/* This file is (c) 2008-2009 Konstantin Isakov <ikm@users.sf.net>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "btreeidx.hh"
#include "folding.hh"
#include "utf8.hh"
#include <math.h>

//#define __BTREE_USE_LZO
// LZO mode is experimental and unsupported. Tests didn't show any substantial
// speed improvements.

#ifdef __BTREE_USE_LZO
#include <lzo/lzo1x.h>

namespace {
struct __LzoInit
{
  __LzoInit()
  {
    lzo_init();
  }
} __lzoInit;
}

#else
#include <zlib.h>
#endif

namespace BtreeIndexing {

enum
{
  BtreeMinElements = 64,
  BtreeMaxElements = 2048
};

BtreeDictionary::BtreeDictionary( string const & id,
                                  vector< string > const & dictionaryFiles ):
  Dictionary::Class( id, dictionaryFiles ), idxFile( 0 )
{
}

void BtreeDictionary::openIndex( File::Class & file )
{
  indexNodeSize = file.read< uint32_t >();
  rootOffset = file.read< uint32_t >();

  idxFile = &file;
}

vector< WordArticleLink > BtreeDictionary::findArticles( wstring const & str )
{
  vector< WordArticleLink > result;

  wstring folded = Folding::apply( str );

  bool exactMatch;

  vector< char > leaf;
  uint32_t nextLeaf;

  char const * chainOffset = findChainOffsetExactOrPrefix( folded, exactMatch,
                                                           leaf, nextLeaf );

  if ( chainOffset && exactMatch )
  {
    result = readChain( chainOffset );

    antialias( str, result );
  }

  return result;
}


void BtreeDictionary::findExact( wstring const & str,
                                 vector< wstring > & exactMatches,
                                 vector< wstring > & prefixMatches,
                                 unsigned long maxPrefixResults )
  throw( std::exception )
{
  exactMatches.clear();
  prefixMatches.clear();

  wstring folded = Folding::apply( str );

  bool exactMatch;

  vector< char > leaf;
  uint32_t nextLeaf;

  char const * chainOffset = findChainOffsetExactOrPrefix( folded, exactMatch,
                                                           leaf, nextLeaf );

  if ( !chainOffset )
    return;

  for( ; ; )
  {
    //printf( "offset = %u, size = %u\n", chainOffset - &leaf.front(), leaf.size() );

    vector< WordArticleLink > chain = readChain( chainOffset );
    vector< wstring > wstrings = convertChainToWstrings( chain );

    wstring resultFolded = Folding::apply( wstrings[ 0 ] );

    if ( resultFolded == folded )
      // Exact match
      exactMatches.insert( exactMatches.end(), wstrings.begin(), wstrings.end() );
    else
    if ( resultFolded.size() > folded.size() && !resultFolded.compare( 0, folded.size(), folded ) )
    {
      // Prefix match
      prefixMatches.insert( prefixMatches.end(), wstrings.begin(), wstrings.end() );

      if ( prefixMatches.size() >= maxPrefixResults )
      {
        // For now we actually allow more than maxPrefixResults if the last
        // chain yield more than one result. That's ok and maybe even more
        // desirable.
        break;
      }
    }
    else
      // No match at all, end this
      break;

    // Fetch new leaf if we're out of chains here

    if ( chainOffset > &leaf.back() )
    {
      // We're past the current leaf, fetch the next one

      //printf( "advancing\n" );

      if ( nextLeaf )
      {
        readNode( nextLeaf, leaf );
        nextLeaf = idxFile->read< uint32_t >();
        chainOffset = &leaf.front() + sizeof( uint32_t );

        uint32_t leafEntries = *(uint32_t *)&leaf.front();

        if ( leafEntries == 0xffffFFFF )
        {
          //printf( "bah!\n" );
          exit( 1 );
        }
      }
      else
        break; // That was the last leaf
    }
  }
}

void BtreeDictionary::readNode( uint32_t offset, vector< char > & out )
{
  idxFile->seek( offset );

  uint32_t uncompressedSize = idxFile->read< uint32_t >();
  uint32_t compressedSize = idxFile->read< uint32_t >();

  //printf( "%x,%x\n", uncompressedSize, compressedSize );

  out.resize( uncompressedSize );

  vector< unsigned char > compressedData( compressedSize );

  idxFile->read( &compressedData.front(), compressedData.size() );

  #ifdef __BTREE_USE_LZO

  lzo_uint decompressedLength = out.size();

  if ( lzo1x_decompress( &compressedData.front(), compressedData.size(),
                         (unsigned char *)&out.front(), &decompressedLength, 0 )
       != LZO_E_OK || decompressedLength != out.size() )
    throw exFailedToDecompressNode();

  #else

  unsigned long decompressedLength = out.size();

  if ( uncompress( (unsigned char *)&out.front(),
                   &decompressedLength,
                   &compressedData.front(),
                   compressedData.size() ) != Z_OK ||
       decompressedLength != out.size() )
    throw exFailedToDecompressNode();
  #endif
}

char const * BtreeDictionary::findChainOffsetExactOrPrefix( wstring const & target,
                                                            bool & exactMatch,
                                                            vector< char > & leaf,
                                                            uint32_t & nextLeaf )
{
  if ( !idxFile )
    throw exIndexWasNotOpened();

  // Lookup the index by traversing the index btree

  vector< char > charBuffer;
  vector< wchar_t > wcharBuffer;
  vector< char > wordsBuffer;

  exactMatch = false;

  // Read a node

  uint32_t currentNodeOffset = rootOffset;

  for( ; ; )
  {
    //printf( "reading node at %x\n", currentNodeOffset );
    readNode( currentNodeOffset, leaf );

    // Is it a leaf or a node?

    uint32_t leafEntries = *(uint32_t *)&leaf.front();

    if ( leafEntries == 0xffffFFFF )
    {
      // A node

      //printf( "=>a node\n" );

      uint32_t const * offsets = (uint32_t *)&leaf.front() + 1;

      char const * ptr = &leaf.front() + sizeof( uint32_t ) +
                         ( indexNodeSize + 1 ) * sizeof( uint32_t );

      unsigned entry;

      for( entry = 0; entry < indexNodeSize; ++entry )
      {
        //printf( "checking node agaist word %s\n", ptr );
        size_t wordSize = strlen( ptr );

        if ( wcharBuffer.size() <= wordSize )
          wcharBuffer.resize( wordSize + 1 );

        long result = Utf8::decode( ptr, wordSize, &wcharBuffer.front() );

        if ( result < 0 )
          throw Utf8::exCantDecode( ptr );

        wcharBuffer[ result ] = 0;

        int compareResult = target.compare( &wcharBuffer.front() );

        if ( !compareResult )
        {
          // The target string matches the current one.
          // Go to the right, since it's there where we store such results.
          currentNodeOffset = offsets[ entry + 1 ];
          break;
        }
        if ( compareResult < 0 )
        {
          // The target string is smaller than the current one.
          // Go to the left.
          currentNodeOffset = offsets[ entry ];
          break;
        }

        ptr += wordSize + 1;
      }

      if ( entry == indexNodeSize )
      {
        // We iterated through all entries, but our string is larger than
        // all of them. Go the the rightmost node.
        currentNodeOffset = offsets[ entry ];
      }
    }
    else
    {
      //printf( "=>a leaf\n" );
      // A leaf
      nextLeaf = idxFile->read< uint32_t >();

      // Iterate through chains until we find one that matches

      char const * ptr = &leaf.front() + sizeof( uint32_t );

      uint32_t chainSize;

      while( leafEntries-- )
      {
        memcpy( &chainSize, ptr, sizeof( uint32_t ) );
        ptr += sizeof( uint32_t );

        if( chainSize )
        {
          size_t wordSize = strlen( ptr );

          if ( wcharBuffer.size() <= wordSize )
            wcharBuffer.resize( wordSize + 1 );

          //printf( "checking agaist word %s, left = %u\n", ptr, leafEntries );

          long result = Utf8::decode( ptr, wordSize, &wcharBuffer.front() );

          if ( result < 0 )
            throw Utf8::exCantDecode( ptr );

          wcharBuffer[ result ] = 0;

          wstring foldedWord = Folding::apply( &wcharBuffer.front() );

          int compareResult = target.compare( foldedWord );

          if ( !compareResult )
          {
            // Exact match -- return and be done
            exactMatch = true;

            return ptr - sizeof( uint32_t );
          }
          else
          if ( compareResult < 0 )
          {
            // The target string is smaller than the current one.
            // No point in travering further, return this result.
            
            return ptr - sizeof( uint32_t );
          }
          ptr += chainSize;
        }
      }

      // Well, our target is larger than all the chains here. This would mean
      // that the next leaf is the right one.

      if ( nextLeaf )
      {
        readNode( nextLeaf, leaf );

        nextLeaf = idxFile->read< uint32_t >();

        return &leaf.front() + sizeof( uint32_t );
      }
      else
        return 0; // This was the last leaf
    }
  }
}

vector< WordArticleLink > BtreeDictionary::readChain( char const * & ptr )
{
  uint32_t chainSize;

  memcpy( &chainSize, ptr, sizeof( uint32_t ) );

  ptr += sizeof( uint32_t );

  vector< WordArticleLink > result;

  vector< char > charBuffer;

  while( chainSize )
  {
    string str = ptr;
    ptr += str.size() + 1;

    uint32_t articleOffset;

    memcpy( &articleOffset, ptr, sizeof( uint32_t ) );

    ptr += sizeof( uint32_t );

    result.push_back( WordArticleLink( str, articleOffset ) );

    if ( chainSize < str.size() + 1 + sizeof( uint32_t ) )
      throw exCorruptedChainData();
    else
      chainSize -= str.size() + 1 + sizeof( uint32_t );
  }

  return result;
}

vector< wstring > BtreeDictionary::convertChainToWstrings(
                                      vector< WordArticleLink > const & chain )
{
  vector< wchar_t > wcharBuffer;

  vector< wstring > result;

  for( unsigned x = 0; x < chain.size(); ++x )
  {
    unsigned wordSize = chain[ x ].word.size();

    if ( wcharBuffer.size() <= wordSize )
      wcharBuffer.resize( wordSize + 1 );

    long len = Utf8::decode( chain[ x ].word.data(), wordSize,
                                &wcharBuffer.front() );

    if ( len < 0 )
    {
      fprintf( stderr, "Failed to decode utf8 of a word %s, skipping it.\n",
               chain[ x ].word.c_str() );
      continue;
    }

    wcharBuffer[ len ] = 0;

    result.push_back( &wcharBuffer.front() );
  }

  return result;
}

void BtreeDictionary::antialias( wstring const & str,
                                 vector< WordArticleLink > & chain )
{
  wstring caseFolded = Folding::applySimpleCaseOnly( str );

  for( unsigned x = chain.size(); x--; )
  {
    // If after applying case folding to each word they wouldn't match, we
    // drop the entry.
    if ( Folding::applySimpleCaseOnly( Utf8::decode( chain[ x ].word ) ) !=
         caseFolded )
      chain.erase( chain.begin() + x );
  }
}


/// A function which recursively creates btree node.
/// The nextIndex iterator is being iterated over and increased when building
/// leaf nodes.
static uint32_t buildBtreeNode( IndexedWords::const_iterator & nextIndex,
                                size_t indexSize,
                                File::Class & file, size_t maxElements,
                                uint32_t & lastLeafLinkOffset )
{
  // We compress all the node data. This buffer would hold it.
  vector< unsigned char > uncompressedData;

  bool isLeaf = indexSize <= maxElements;

  if ( isLeaf )
  {
    // A leaf.

    uint32_t totalChainsLength = 0;

    IndexedWords::const_iterator nextWord = nextIndex;

    for( unsigned x = indexSize; x--; ++nextWord )
    {
      totalChainsLength += sizeof( uint32_t );

      vector< WordArticleLink > const & chain = nextWord->second;

      for( unsigned y = 0; y < chain.size(); ++y )
        totalChainsLength += chain[ y ].word.size() + 1 + sizeof( uint32_t );
    }

    uncompressedData.resize( sizeof( uint32_t ) + totalChainsLength );

    // First uint32_t indicates that this is a leaf.
    *(uint32_t *)&uncompressedData.front() = indexSize;

    unsigned char * ptr = &uncompressedData.front() + sizeof( uint32_t );

    for( unsigned x = indexSize; x--; ++nextIndex )
    {
      vector< WordArticleLink > const & chain = nextIndex->second;

      unsigned char * saveSizeHere = ptr;

      ptr += sizeof( uint32_t );

      uint32_t size = 0;

      for( unsigned y = 0; y < chain.size(); ++y )
      {
        memcpy( ptr, chain[ y ].word.c_str(), chain[ y ].word.size() + 1 );
        ptr += chain[ y ].word.size() + 1;

        memcpy( ptr, &(chain[ y ].articleOffset), sizeof( uint32_t ) );
        ptr += sizeof( uint32_t );

        size += chain[ y ].word.size() + 1 + sizeof( uint32_t );
      }

      memcpy( saveSizeHere, &size, sizeof( uint32_t ) );
    }
  }
  else
  {
    // A node which will have children.

    uncompressedData.resize( sizeof( uint32_t ) + ( maxElements + 1 ) * sizeof( uint32_t ) );

    // First uint32_t indicates that this is a node.
    *(uint32_t *)&uncompressedData.front() = 0xffffFFFF;

    unsigned prevEntry = 0;

    vector< char > charBuffer;

    for( unsigned x = 0; x < maxElements; ++x )
    {
      unsigned curEntry = (uint64_t) indexSize * ( x + 1 ) / ( maxElements + 1 );

      uint32_t offset = buildBtreeNode( nextIndex,
                                        curEntry - prevEntry,
                                        file, maxElements,
                                        lastLeafLinkOffset );

      memcpy( &uncompressedData.front() + sizeof( uint32_t ) + x * sizeof( uint32_t ), &offset, sizeof( uint32_t ) );

      if ( charBuffer.size() < nextIndex->first.size() * 4 )
        charBuffer.resize( nextIndex->first.size() * 4 );

      size_t sz = Utf8::encode( nextIndex->first.data(), nextIndex->first.size(),
                                &charBuffer.front() );

      size_t prevSize = uncompressedData.size();
      uncompressedData.resize( prevSize + sz + 1 );

      memcpy( &uncompressedData.front() + prevSize, &charBuffer.front(), sz );

      uncompressedData.back() = 0;

      prevEntry = curEntry;
    }

    // Rightmost child
    uint32_t offset = buildBtreeNode( nextIndex,
                                      indexSize - prevEntry,
                                      file, maxElements,
                                      lastLeafLinkOffset );
    memcpy( &uncompressedData.front() + sizeof( uint32_t ) +
            maxElements * sizeof( uint32_t ), &offset, sizeof( offset ) );
  }

  // Save the result.

  #ifdef __BTREE_USE_LZO

  vector< unsigned char > compressedData( uncompressedData.size() + uncompressedData.size() / 16 + 64 + 3 );

  char workMem[ LZO1X_1_MEM_COMPRESS ];

  lzo_uint compressedSize;

  if ( lzo1x_1_compress( &uncompressedData.front(), uncompressedData.size(),
                         &compressedData.front(), &compressedSize, workMem )
       != LZO_E_OK )
  {
    fprintf( stderr, "Failed to compress btree node.\n" );
    abort();
  }

  #else

  vector< unsigned char > compressedData( compressBound( uncompressedData.size() ) );

  unsigned long compressedSize = compressedData.size();

  if ( compress( &compressedData.front(), &compressedSize,
                 &uncompressedData.front(), uncompressedData.size() ) != Z_OK )
  {
    fprintf( stderr, "Failed to compress btree node.\n" );
    abort();
  }

  #endif

  uint32_t offset = file.tell();

  file.write< uint32_t >( uncompressedData.size() );
  file.write< uint32_t >( compressedSize );
  file.write( &compressedData.front(), compressedSize );

  if ( isLeaf )
  {
    // A link to the next leef, which is zero and which will be updated
    // should we happen to have another leaf.
    
    file.write( ( uint32_t ) 0 );

    uint32_t here = file.tell();

    if ( lastLeafLinkOffset )
    {
      // Update the previous leaf to have the offset of this one.
      file.seek( lastLeafLinkOffset );
      file.write( offset );
      file.seek( here );
    }

    // Make sure next leaf knows where to write its offset for us.
    lastLeafLinkOffset = here - sizeof( uint32_t );
  }

  return offset;
}

uint32_t buildIndex( IndexedWords const & indexedWords, File::Class & file )
{
  // We try to stick to two-level tree for most dictionaries. Try finding
  // the right size for it.

  size_t btreeMaxElements = ( (size_t) sqrt( indexedWords.size() ) ) + 1;

  if ( btreeMaxElements < BtreeMinElements )
    btreeMaxElements = BtreeMinElements;
  else
  if ( btreeMaxElements > BtreeMaxElements )
    btreeMaxElements = BtreeMaxElements;

  printf( "Building a tree of %u elements\n", btreeMaxElements );

  IndexedWords::const_iterator nextIndex = indexedWords.begin();

  uint32_t lastLeafOffset = 0;

  uint32_t rootOffset = buildBtreeNode( nextIndex, indexedWords.size(),
                                        file, btreeMaxElements,
                                        lastLeafOffset );

  // We need to save btreeMaxElements. For simplicity, we just save it here
  // along with root offset, and then return that record's offset as the
  // offset of the index itself.

  uint32_t indexOffset = file.tell();

  file.write( btreeMaxElements );
  file.write( rootOffset );

  return indexOffset;
}


}