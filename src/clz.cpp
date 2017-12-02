// Copyright (c) 2017 CIYAM Developers
//
// Distributed under the MIT/X11 software license, please refer to the file license.txt
// in the root project directory or http://www.opensource.org/licenses/mit-license.php.

#ifdef PRECOMPILE_H
#  include "precompile.h"
#endif
#pragma hdrstop

#ifndef HAS_PRECOMPILED_STD_HEADERS
#  include <cstring>
#  include <map>
#  include <set>
#  include <deque>
#  include <vector>
#  include <string>
#  include <memory>
#  include <fstream>
#  include <iomanip>
#  include <iostream>
#endif

#include "clz.h"

//#define DEBUG

#ifdef DEBUG
#  define DEBUG_DECODE
#  define DEBUG_ENCODE
#endif

//#define COMPILE_TESTBED_MAIN

using namespace std;

namespace
{

// NOTE: This custom LZ encoding/decoding approach works with a 4KB chunk size
// although the exact number of bytes used can generally be less due to simple
// sequence and pattern repeat shrinking that is performed prior to outputting
// a chunk. Data values are 7-bit with the MSB of each byte being reserved for
// indicating either a back-ref or a "special" shrunken pattern/sequence. Each
// back-ref or back-ref repeat consists of two bytes whose bit values are used
// as follows:
//
// [byte 1] [byte 2]
// 1xxxyyyy yyyyyyyy
//
// where:   xxx = 2^3 length (3-9) 0xF is being reserved for last pair repeats
// yyyyyyyyyyyy = 2^12 offset (0-4095) or 2^11 last pair repeat value (1-2048)
//
// Back-ref repeats are differentiated by starting with 0xF (thus all back-ref
// lengths must be between 3 and 9 which are the values 0x8..0xE). In order to
// not confuse back-ref repeats with special shrunk pattern/sequences they are
// limited to a maximum of 2048 (with 0 being for 1 repeat and 0x7FF being for
// 2048 repeats). The leading values 0xFA..0xFF are being reserved for special
// patterns which can occur in place of any normal back-ref or back-ref repeat
// value. Because back-ref repeats can only occur immediately after a back-ref
// the leading values 0xF0..0xF7 are reserved for simple character repeats (to
// repeat between 1 and 8 times) with the leading value 0xF8 being reserved as
// a special marker and 0xF9 being reserved for nibble/byte sequences.

const size_t c_max_offset = 4095;
const size_t c_max_repeats = 2048;

const size_t c_max_combines = 5;

const size_t c_min_pat_length = 3;
const size_t c_max_pat_length = 9;

const size_t c_meta_pat_length = 4;

const size_t c_max_encoded_chunk_size = c_max_offset + c_min_pat_length + 2;

const size_t c_max_specials = 6;
const size_t c_max_special_repeats = 8;
const size_t c_max_special_step_vals = 15;

const unsigned char c_nibble_one = 0xf0;
const unsigned char c_nibble_two = 0x0f;

const unsigned char c_high_bit_value = 0x80;
const unsigned char c_high_five_bits = 0xf8;

const unsigned char c_max_repeats_hi = 0xf7;
const unsigned char c_max_repeats_lo = 0xff;

const unsigned char c_special_marker = 0xf8;
const unsigned char c_special_nsteps = 0xf9;
const unsigned char c_special_maxval = 0xff;

#ifdef DEBUG
void dump_bytes( const char* p_prefix, unsigned char* p_input, size_t num, size_t mark = 0 )
{
   cout << p_prefix << hex;

   for( size_t i = 0; i < num; i++ )
   {
      if( mark && i == mark )
         cout << '|';

      cout << setw( 2 ) << setfill( '0' ) << ( int )p_input[ i ];
   }

   cout << dec << endl;
}
#endif

struct repeat_info
{
   repeat_info( ) : offset( 0 ), length( 0 ) { }

   size_t offset;
   size_t length;
};

typedef pair< unsigned char, unsigned char > byte_pair;

struct meta_pattern
{
   unsigned char byte1;
   unsigned char byte2;
   unsigned char byte3;
   unsigned char byte4;

   bool operator <( const meta_pattern& p ) const
   {
      bool retval = false;

      if( byte1 < p.byte1 )
         retval = true;
      else if( byte1 == p.byte1 )
      {
         if( byte2 < p.byte2 )
            retval = true;
         else if( byte2 == p.byte2 )
         {
            if( byte3 < p.byte3 )
               retval = true;
            else if( byte3 == p.byte3 )
               retval = byte4 < p.byte4;
         }
      }

      return retval;
   }

   bool operator ==( const meta_pattern& p ) const
   {
      return byte1 == p.byte1 && byte2 == p.byte2 && byte3 == p.byte3 && byte4 == p.byte4;
   }
};

typedef byte_pair meta_pair;

ostream& operator <<( ostream& os, const meta_pair& p )
{
   os << hex
    << setw( 2 ) << setfill( '0' ) << ( int )p.first << setw( 2 ) << setfill( '0' ) << ( int )p.second << dec;

   return os;
}

ostream& operator <<( ostream& os, const meta_pattern& p )
{
   os << hex
    << setw( 2 ) << setfill( '0' ) << ( int )p.byte1 << setw( 2 ) << setfill( '0' ) << ( int )p.byte2
    << setw( 2 ) << setfill( '0' ) << ( int )p.byte3 << setw( 2 ) << setfill( '0' ) << ( int )p.byte4 << dec;

   return os;
}

struct meta_pattern_info
{
   void clear( )
   {
      offsets.clear( );
      patterns.clear( );
   }

   bool has_offset( size_t offset ) { return offsets.count( offset ); }

   bool has_pattern( const meta_pattern& pat ) { return patterns.count( pat ); }

   void add_pattern( const meta_pattern& pat, size_t offset )
   {
      offsets[ offset ] = pat;

      patterns[ pat ].first = 0x90 | ( ( offset & 0x0f00 ) >> 8 );
      patterns[ pat ].second = ( offset & 0x00ff );
#ifdef DEBUG_ENCODE
cout << "add pattern: " << patterns[ pat ] << " ==> " << pat << " @" << offset << endl;
#endif
   }

   meta_pair operator [ ]( const meta_pattern& pat )
   {
      return patterns[ pat ];
   }

   meta_pattern operator [ ]( size_t offset )
   {
      return offsets[ offset ];
   }

   size_t last_offset( ) const
   {
      size_t val = 0;

      map< size_t, meta_pattern >::const_iterator ci = offsets.end( );

      if( !offsets.empty( ) )
         val = ( --ci )->first;
      
      return val;
   }

   size_t pattern_offset( const meta_pattern& pat )
   {
      size_t val = 0;

      if( has_pattern( pat ) )
      {
         for( map< size_t, meta_pattern >::iterator i = offsets.begin( ); i != offsets.end( ); ++i )
         {
            if( i->second == pat )
            {
               val = i->first;
               break;
            }
         }
      }

      return val;
   }

   void remove_at_offset( size_t offset )
   {
      if( has_offset( offset ) )
      {
         meta_pattern pat = offsets[ offset ];
         offsets.erase( offset );

#ifdef DEBUG_ENCODE
cout << "rem pattern: " << patterns[ pat ] << " ==> " << pat << " @" << offset << endl;
#endif
         patterns.erase( pat );
      }
   }

   void remove_offsets_from( size_t start )
   {
      while( true )
      {
         size_t next = last_offset( );

         if( next < start )
            break;

         remove_at_offset( next );
      }
   }

   map< size_t, meta_pattern > offsets;
   map< meta_pattern, meta_pair > patterns;
};

void check_meta_patterns( meta_pattern_info& meta_patterns, unsigned char* p_buffer, size_t offset )
{
   for( size_t i = 0; i < offset; i++ )
   {
      if( meta_patterns.has_offset( i ) )
      {
         meta_pattern pat = meta_patterns[ i ];

         size_t len = ( ( meta_patterns[ pat ].first & c_nibble_one ) >> 4 ) - 8 + 3;
         size_t offset = ( ( meta_patterns[ pat ].first & c_nibble_two ) << 8 ) + meta_patterns[ pat ].second;

         if( pat.byte1 != *( p_buffer + offset ) || pat.byte2 != *( p_buffer + offset + 1 )
          || pat.byte3 != *( p_buffer + offset + 2 ) || pat.byte4 != *( p_buffer + offset + 3 ) )
            cout << "*** found invalid meta pattern @" << offset << " ***" << endl;
#ifdef DEBUG_ENCODE
cout << meta_patterns[ pat ] << " for " << pat << " at @" << offset << endl;
#endif
      }
   }
}

bool found_stepping_nibbles( unsigned char* p_buffer, size_t offset, size_t length, size_t& nibbles, bool& ascending )
{
   size_t step_amount = 0;

   for( nibbles = 1; nibbles <= 4; nibbles++ )
   {
      if( nibbles == 1 && offset + 3 < length )
      {
         unsigned char ch = *( p_buffer + offset );

         unsigned char nibble1 = ( ch & c_nibble_one ) >> 4;
         unsigned char nibble2 = ch & c_nibble_two;

         if( nibble1 == nibble2 )
            continue;

         ascending = ( nibble1 < nibble2 ? true : false );
         step_amount = ( ascending ? nibble2 - nibble1 : nibble1 - nibble2 );

         bool found = true;

         for( size_t j = 1; j < 4; j++ )
         {
            ch = *( p_buffer + offset + j );

            unsigned char new_nibble1 = ( ch & c_nibble_one ) >> 4;
            unsigned char new_nibble2 = ch & c_nibble_two;

            if( ( ascending && new_nibble1 != nibble2 + step_amount )
             || ( !ascending && new_nibble1 != nibble2 - step_amount )
             || ( ascending && new_nibble2 != new_nibble1 + step_amount )
             || ( !ascending && new_nibble2 != new_nibble1 - step_amount ) )
            {
               found = false;
               break;
            }

            nibble1 = new_nibble1;
            nibble2 = new_nibble2;
         }

         if( found )
            break;
         else
            step_amount = 0;
      }
      else if( nibbles == 2 && offset + 4 < length )
      {
         unsigned char byte1 = *( p_buffer + offset );
         unsigned char byte2 = *( p_buffer + offset + 1 );

         if( byte1 == byte2 )
            continue;

         ascending = ( byte1 < byte2 ? true : false );
         step_amount = ( ascending ? byte2 - byte1 : byte1 - byte2 );

         unsigned char byte3 = *( p_buffer + offset + 2 );
         unsigned char byte4 = *( p_buffer + offset + 3 );

         bool found = true;

         if( ( ascending && byte3 != byte2 + step_amount )
          || ( !ascending && byte3 != byte2 - step_amount )
          || ( ascending && byte4 != byte3 + step_amount )
          || ( !ascending && byte4 != byte3 - step_amount ) )
            found = false;
         else
         {
            unsigned char byte5 = *( p_buffer + offset + 4 );

            if( ( ascending && byte5 != byte4 + step_amount )
             || ( !ascending && byte5 != byte4 - step_amount ) )
               found = false;
         }

         if( found )
            break;
         else
            step_amount = 0;
      }
      // FUTURE: Should check for patterns of 3 and 4 nibbles also.
   }

   return step_amount;
}

bool shrink_output( unsigned char* p_buffer, size_t& length )
{
   unsigned char shrunken[ c_max_encoded_chunk_size ];

   if( length <= c_max_encoded_chunk_size )
   {
      map< byte_pair, size_t > pairs;

      // NOTE: A "byte pair" is either a back-ref, a meta-pattern
      // or a pair repeat value (each of which will start with an
      // MSB set in the first byte). Thus pairs are being counted
      // here to find any repeats that will be candidates for the
      // "specials".
      for( size_t i = 0; i < length; i++ )
      {
         unsigned char next = *( p_buffer + i );

         if( i < length - 1 && ( next & c_high_bit_value ) )
         {
            byte_pair next_pair;

            next_pair.first = next;
            next_pair.second = *( p_buffer + ++i );

            ++pairs[ next_pair ];
         }
      }

      // NOTE: Only pairs with 3 or more repeats can be considered as "specials" so firstly
      // remove those that don't qualify.
      set< byte_pair > worst;
      for( map< byte_pair, size_t >::iterator pi = pairs.begin( ); pi != pairs.end( ); ++pi )
      {
         if( pi->second <= 2 )
            worst.insert( pi->first );
      }

      for( set< byte_pair >::iterator wi = worst.begin( ); wi != worst.end( ); ++wi )
         pairs.erase( *wi );

      multimap< size_t, byte_pair > ordered;

      // NOTE: Order the pairs and then remove the least repeated ones if there are more of
      // these than the maximum number of specials permitted and then number all remaining.
      for( map< byte_pair, size_t >::iterator pi = pairs.begin( ); pi != pairs.end( ); ++pi )
         ordered.insert( make_pair( pi->second, pi->first ) );

      while( ordered.size( ) > c_max_specials )
         ordered.erase( ordered.begin( ) );

      set< byte_pair > specials;

      for( multimap< size_t, byte_pair >::iterator oi = ordered.begin( ); oi != ordered.end( ); ++oi )
         specials.insert( oi->second );

      map< byte_pair, size_t > special_nums;

      for( set< byte_pair >::iterator si = specials.begin( ); si != specials.end( ); ++si )
         special_nums[ *si ] = special_nums.size( ) - 1;

      size_t num = 0;
      size_t repeats = 0;

      bool steps_ascending = true;

      size_t stepping_amount = 0;
      size_t stepping_nibbles = 0;

      size_t last_special_pos = 0;

      unsigned char last_ch = c_special_maxval;

      size_t available_specials = ( c_max_specials - special_nums.size( ) );

      map< byte_pair, size_t > repeated_special_counts;
      map< size_t, byte_pair > repeated_special_offsets;

      for( size_t i = 0; i < length; i++ )
      {
         unsigned char next = *( p_buffer + i );

         if( stepping_amount )
         {
            size_t next_val = last_ch;

            if( stepping_nibbles == 1 )
               next_val = ( last_ch & c_nibble_one ) >> 4;

            if( steps_ascending )
               next_val += stepping_amount;
            else
               next_val -= stepping_amount;

            if( stepping_nibbles == 1 )
            {
               next_val <<= 4;
               next_val += ( last_ch & c_nibble_two );

               if( steps_ascending )
                  next_val += stepping_amount;
               else
                  next_val -= stepping_amount;
            }

            if( next == next_val && ( shrunken[ num - 1 ] & c_nibble_two ) < c_max_special_step_vals )
            {
               ++shrunken[ num - 1 ];
               last_ch = next;
               continue;
            }
            else
               stepping_amount = 0;
         }

         if( next != last_ch && repeats )
         {
            shrunken[ num++ ] = ( c_nibble_one + repeats - 1 );
            repeats = 0;
         }

         // NOTE: If a byte pair had been identified as a special pair then
         // append its special marker otherwise simply append the values.
         if( i < length - 1 && ( next & c_high_bit_value ) )
         {
            byte_pair next_pair;

            next_pair.first = next;
            next_pair.second = *( p_buffer + ++i );

            if( special_nums.count( next_pair ) )
            {
               shrunken[ num ] = ( c_special_maxval - special_nums[ next_pair ] );

               if( available_specials && last_special_pos == num - 1 )
               {
                  bool add_new_pair = true;

                  byte_pair new_pair( shrunken[ num - 1 ], shrunken[ num ] );

                  // NOTE: As runs of three (or more) specials are entirely possible overlaps
                  // could end up occurring, however, assuming the run "fefdfe" then in order
                  // to decide whether to use "fefd" or "fdfe" first just add "fefd" but then
                  // if "fdfe" had appeared more times previously then remove the last "fefd"
                  // before adding the new "fdfe" (this doesn't take into account what is yet
                  // to come so it could end up making a worser choice unfortunately).
                  if( repeated_special_offsets.count( num - 2 ) )
                  {
                     if( repeated_special_counts.count( new_pair )
                      < repeated_special_counts.count( repeated_special_offsets[ num - 2 ] ) )
                        add_new_pair = false;
                     else
                     {
                        --repeated_special_counts[ repeated_special_offsets[ num - 2 ] ];
                        repeated_special_offsets.erase( num - 2 );
                     }
                  }

                  if( add_new_pair )
                  {
                     ++repeated_special_counts[ new_pair ];
                     repeated_special_offsets[ num - 1 ] = new_pair;
                  }
               }

               last_special_pos = num++;
            }
            else
            {
               shrunken[ num++ ] = next_pair.first;
               shrunken[ num++ ] = next_pair.second;
            }

            last_ch = c_special_maxval;
         }
         else
         {
            // NOTE: Simple characters that repeated are shrunk with a single byte
            // to indicate this along with the number of repeats (one nibble each).
            if( next == last_ch && repeats < c_max_special_repeats - 1 )
               ++repeats;
            else
            {
               bool found_steps = false;

               // NOTE: If groups of nibbles are found to be in a run of incrementing or decrementing steps then
               // these can be shrunk also.
               if( i < length - 3 )
               {
                  stepping_amount = found_stepping_nibbles( p_buffer, i, length, stepping_nibbles, steps_ascending );

                  if( stepping_amount )
                  {
                     found_steps = true;

                     shrunken[ num++ ] = next;
                     shrunken[ num++ ] = last_ch = *( p_buffer + ++i );

                     shrunken[ num++ ] = c_special_nsteps;
                     shrunken[ num++ ] = ( stepping_nibbles - 1 ) << 4;
                  }
               }

               if( !found_steps )
                  shrunken[ num++ ] = last_ch = next;
            }
         }
      }

      if( repeats )
         shrunken[ num++ ] = ( c_nibble_one + repeats - 1 );

      vector< byte_pair > extra_specials;

      if( !repeated_special_counts.empty( ) )
      {
         map< size_t, byte_pair > ordered;

         for( map< byte_pair, size_t >::iterator
          i = repeated_special_counts.begin( ); i != repeated_special_counts.end( ); ++i )
         {
            if( i->second > 2 )
               ordered.insert( make_pair( i->second, i->first ) );
         }

         while( ordered.size( ) > available_specials )
            ordered.erase( ordered.begin( ) );

         set< byte_pair > repeated_specials;
         map< byte_pair, size_t > repeated_specials_used;

         // NOTE: If there is only one potential extra special and it
         // only has 3 repeats then it isn't worth the effort (due to
         // the need for a marker byte when using extra specials).
         if( ordered.size( ) == 1 && ordered.begin( )->first == 3 )
         {
            ordered.clear( );
            repeated_special_offsets.clear( );
         }

         for( map< size_t, byte_pair >::iterator oi = ordered.begin( ); oi != ordered.end( ); ++oi )
            repeated_specials.insert( oi->second );

         size_t already_adjusted = 0;

         // NOTE: Any pairs of specials that had been repeated three or more times can also
         // become specials (called "extra-specials") if the maximum number of specials had
         // not been already used. Such replacements require memmove's to occur as they are
         // being done after the fact rather than when initially compressing as occurs with
         // the "normal" specials.
         for( map< size_t, byte_pair >::iterator
          i = repeated_special_offsets.begin( ); i != repeated_special_offsets.end( ); ++i )
         {
            if( repeated_specials.count( i->second ) )
            {
               size_t special_num = specials.size( );

               if( repeated_specials_used.count( i->second ) )
                  special_num = repeated_specials_used[ i->second ];
               else
               {
                  special_num += extra_specials.size( );
                  repeated_specials_used[ i->second ] = special_num;

                  extra_specials.push_back( i->second );
               }

               size_t offset = ( i->first - already_adjusted );

               shrunken[ offset ] = ( c_special_maxval - special_num );
               memmove( &shrunken[ offset + 1 ], &shrunken[ offset + 2 ], num - offset );

               --num;
               ++already_adjusted;
            }
         }
      }

      // NOTE: The specials are appended at the end - and as only special markers start with
      // all five high bits set (i.e. 0xf8) the number of these used can be determined while
      // reading the input (knowing the maximum number of block bytes and accounting for the
      // number of different special markers that are found).
      for( set< byte_pair >::iterator si = specials.begin( ); si != specials.end( ); ++si )
      {
         shrunken[ num++ ] = si->first;
         shrunken[ num++ ] = si->second;
      }

      if( extra_specials.size( ) )
         shrunken[ num++ ] = c_special_marker;

      for( size_t i = 0; i < extra_specials.size( ); i++ )
      {
         shrunken[ num++ ] = extra_specials[ i ].first;
         shrunken[ num++ ] = extra_specials[ i ].second;
      }

      if( num < length )
      {
         memcpy( p_buffer, shrunken, length = num );
#ifdef DEBUG_ENCODE
         dump_bytes( "shrunken ==> ", shrunken, num );
#endif
      }
   }
}

size_t expand_input( istream& is, unsigned char* p_buffer, size_t max_length )
{
   size_t length = 0;
   size_t skip_count = 0;
   size_t num_specials = 0;

   unsigned char last_ch = 0;

   bool had_marker = false;
   bool process_steps = false;

   set< size_t > back_refs;
   map< size_t, size_t > specials;

   memset( p_buffer, 0, max_length );

   while( is )
   {
      unsigned char ch;

      if( !is.read( ( char* )&ch, 1 ) )
         break;

      if( skip_count )
      {
         *( p_buffer + length++ ) = ch;
         --skip_count;
         continue;
      }

      if( process_steps )
      {
         process_steps = false;

         bool ascending = true;

         size_t nibbles = ( ( ch & c_nibble_one ) >> 4 ) + 1;
         size_t num_repeats = ( ch & c_nibble_two );

         size_t stepping_amount = 0;

         if( nibbles == 1 )
         {
            unsigned char nibble1 = ( *( p_buffer + length - 1 ) & c_nibble_one ) >> 4;
            unsigned char nibble2 = ( *( p_buffer + length - 1 ) & c_nibble_two );

            ascending = ( nibble1 < nibble2 ? true : false );
            stepping_amount = ( ascending ? nibble2 - nibble1 : nibble1 - nibble2 );

            stepping_amount = ( stepping_amount << 4 ) + stepping_amount;
         }
         else if( nibbles == 2 )
         {
            unsigned char byte1 = *( p_buffer + length - 2 );
            unsigned char byte2 = *( p_buffer + length - 1 );

            ascending = ( byte1 < byte2 ? true : false );
            stepping_amount = ( ascending ? byte2 - byte1 : byte1 - byte2 ); 
         }

         for( size_t i = 0; i < num_repeats; i++ )
            *( p_buffer + length ) = *( p_buffer + length++ - 1 ) + stepping_amount;

         continue;
      }

      if( had_marker )
         *( p_buffer + length ) = ch;
      else
      {
         if( ( ch & c_high_five_bits ) == c_high_five_bits && !back_refs.count( length - 1 ) )
         {
            // NOTE: Expand either a simple repeated value or step repeated values.
            if( ch == c_special_marker )
            {
               had_marker = true;
               continue;
            }
            else if( ch == c_special_nsteps )
            {
               process_steps = true;
               continue;
            }
            else
            {
               specials[ length++ ] = ( c_special_maxval - ch );

               if( ( c_special_maxval - ch ) + 1 > num_specials )
                  num_specials = ( c_special_maxval - ch ) + 1;
            }
         }
         else
         {
            // NOTE: The "back_refs" container here is used to hold both back-refs
            // and back-ref repeat values (so these are not confused with specials
            // or single character repeats).
            if( ( ch & c_high_bit_value )
             && !back_refs.count( length - 1 )
             && ( ( ( ch & c_nibble_one ) != c_nibble_one )
             || back_refs.count( length - 2 ) || specials.count( length - 2 ) ) )
               back_refs.insert( length );

            bool was_expanded_literal = false;

            if( ( ( ch & c_nibble_one ) == c_nibble_one ) )
            {
               if( !back_refs.count( length - 1 )
                && !back_refs.count( length - 2 ) && !specials.count( length - 2 ) )
               {
                  was_expanded_literal = true;

                  for( size_t i = 0; i <= ( ch - c_nibble_one ); i++ )
                     *( p_buffer + length++ ) = last_ch;

                  --length; // NOTE: Due to the increment below.
               }
            }

            last_ch = ch;

            if( !was_expanded_literal )
               *( p_buffer + length ) = ch;
         }
      }

      if( ++length >= max_length )
         break;
   }

   if( num_specials )
   {
      vector< byte_pair > special_pairs;

      size_t specials_offset = length - ( num_specials * 2 );

      // NOTE: Move the specials into a vector otherwise the buffer could potentially be
      // overrun due to extra special expansion memmove operations.
      for( size_t i = 0; i < num_specials; i++ )
      {
         special_pairs.push_back( byte_pair(
          *( p_buffer + specials_offset + ( i * 2 ) ),
          *( p_buffer + specials_offset + ( i * 2 ) + 1 ) ) );
      }

      length = specials_offset;

      size_t already_adjusted = 0;

      for( map< size_t, size_t >::iterator i = specials.begin( ); i != specials.end( ); ++i )
      {
         size_t offset = ( i->first + already_adjusted );

         *( p_buffer + offset ) = special_pairs[ i->second ].first;
         *( p_buffer + offset + 1 ) = special_pairs[ i->second ].second;

         // NOTE: If extra specials were used then need to expand them as two normal specials
         // along with doing a memmove to make room for the doubled expansion.
         if( ( *( p_buffer + offset ) & c_high_five_bits ) == c_special_marker )
         {
            size_t num_1 = c_special_maxval - *( p_buffer + offset );
            size_t num_2 = c_special_maxval - *( p_buffer + offset + 1 );

            memmove( p_buffer + offset + 2, p_buffer + offset, length - offset );

            length += 2;
            already_adjusted += 2;

            *( p_buffer + offset ) = special_pairs[ num_1 ].first;
            *( p_buffer + offset + 1 ) = special_pairs[ num_1 ].second;
            *( p_buffer + offset + 2 ) = special_pairs[ num_2 ].first;
            *( p_buffer + offset + 3 ) = special_pairs[ num_2 ].second;
         }
      }
   }

#ifdef DEBUG_DECODE
dump_bytes( "expanded ==> ", p_buffer, length );
#endif
   return length;
}

bool combine_meta_patterns( meta_pattern_info& meta_patterns, unsigned char* p_buffer, size_t& offset, size_t& last_pattern_offset )
{
   bool can_continue = false;

   if( offset > ( c_min_pat_length + c_meta_pat_length ) )
   {
      meta_pattern pat;

      pat.byte1 = *( p_buffer + offset - 4 );
      pat.byte2 = *( p_buffer + offset - 3 );
      pat.byte3 = *( p_buffer + offset - 2 );
      pat.byte4 = *( p_buffer + offset - 1 );

      if( ( pat.byte1 & c_nibble_one ) != c_nibble_one )
      {
         // NOTE: Firstly handle a simple pattern replace.
         if( meta_patterns.has_pattern( pat ) && meta_patterns.pattern_offset( pat ) < offset - 4 )
         {
            meta_patterns.remove_offsets_from( offset - 6 );

            *( p_buffer + offset - 4 ) = meta_patterns[ pat ].first;
            *( p_buffer + offset - 3 ) = meta_patterns[ pat ].second;
#ifdef DEBUG_ENCODE
cout << "replaced " << pat << " @" << ( offset - 4 ) << " with: " << meta_patterns[ pat ] << " (combine)" << endl;
#endif

            offset -= 2;
            can_continue = true;
            last_pattern_offset = ( offset - 2 );
         }
         // NOTE: Secondly handle a simple pattern repeat.
         else if( pat.byte1 == pat.byte3 && pat.byte2 == pat.byte4 )
         {
            meta_patterns.remove_offsets_from( offset - 4 );

            meta_pattern rpl( pat );

            rpl.byte3 = c_nibble_one;
            rpl.byte4 = 0x00;

            *( p_buffer + offset - 2 ) = rpl.byte3;
            *( p_buffer + offset - 1 ) = rpl.byte4;
#ifdef DEBUG_ENCODE
cout << "replaced " << pat << " @" << ( offset - 4 ) << " with: " << rpl << " (repeats)" << endl;
#endif

            if( !meta_patterns.has_pattern( rpl ) )
               meta_patterns.add_pattern( rpl, offset - 4 );

            can_continue = true;
            last_pattern_offset = ( offset - 4 );
         }
         // NOTE: Handle the combination of two patterns.
         else if( meta_patterns.has_offset( offset - 6 )
          && meta_patterns.has_pattern( pat ) && meta_patterns.pattern_offset( pat ) < offset - 4 )
         {
            meta_patterns.remove_offsets_from( offset - 6 );

            *( p_buffer + offset - 4 ) = meta_patterns[ pat ].first;
            *( p_buffer + offset - 3 ) = meta_patterns[ pat ].second;

            pat.byte1 = *( p_buffer + offset - 6 );
            pat.byte2 = *( p_buffer + offset - 5 );
            pat.byte3 = *( p_buffer + offset - 4 );
            pat.byte4 = *( p_buffer + offset - 3 );
#ifdef DEBUG_ENCODE
cout << "combined @" << ( offset - 4 ) << " with: " << pat << endl;
dump_bytes( "", p_buffer, offset );
#endif

            if( !meta_patterns.has_pattern( pat ) )
               meta_patterns.add_pattern( pat, offset - 4 );

            offset -= 2;
            can_continue = true;
            last_pattern_offset = ( offset - 2 );
         }
         else
         {
            // NOTE: If the first two bytes part of the current meta-pattern points
            // to an existing meta-pattern then possibly add as a new meta-pattern.
            if( ( pat.byte1 & c_nibble_one ) == 0x90 && !meta_patterns.has_pattern( pat ) )
            {
               size_t first_offset = ( ( pat.byte1 & c_nibble_two ) << 8 ) + pat.byte2;

               if( meta_patterns.has_offset( first_offset ) )
               {
                  meta_patterns.remove_offsets_from( offset - 4 );
                  meta_patterns.add_pattern( pat, offset - 4 );
               }
            }
         }
      }
      else
      {
         if( pat.byte3 == *( p_buffer + offset - 6 ) && pat.byte4 == *( p_buffer + offset - 5 ) )
         {
            meta_patterns.remove_offsets_from( offset - 6 );

            if( *( p_buffer + offset - 4 ) != c_max_repeats_hi
             || *( p_buffer + offset - 3 ) != c_max_repeats_lo )
            {
               if( *( p_buffer + offset - 3 ) != c_max_repeats_lo )
                  ++( *( p_buffer + offset - 3 ) );
               else
               {
                  *( p_buffer + offset - 3 ) = 0;
                  ++( *( p_buffer + offset - 4 ) );
               }

               pat.byte1 = *( p_buffer + offset - 6 );
               pat.byte2 = *( p_buffer + offset - 5 );
               pat.byte3 = *( p_buffer + offset - 4 );
               pat.byte4 = *( p_buffer + offset - 3 );
#ifdef DEBUG_ENCODE
cout << "inc removed @" << ( offset - 6 ) << " with: " << pat << endl;
#endif

               if( !meta_patterns.has_pattern( pat ) )
                  meta_patterns.add_pattern( pat, offset - 6 );

               offset -= 2;
               can_continue = true;
               last_pattern_offset = ( offset - 4 );
            }
         }
         else
         {
            meta_pattern rpt;

            rpt.byte1 = *( p_buffer + offset - 6 );
            rpt.byte2 = *( p_buffer + offset - 5 );
            rpt.byte3 = *( p_buffer + offset - 4 );
            rpt.byte4 = *( p_buffer + offset - 3 );

            // NOTE: As the repeat is followed by an unrelated pattern now check to see if a pattern
            // that combines the earlier pattern and its repeat amount had been added earlier (prior
            // to the original patterns offset as its repeat may have been the first such occurrence
            // of the combined pattern and repeat) and if so replace and reduce the offset.
            if( meta_patterns.has_pattern( rpt )
             && meta_patterns.pattern_offset( rpt ) < ( offset - 6 ) )
            {
               meta_patterns.remove_offsets_from( offset - 8 );

               *( p_buffer + offset - 6 ) = rpt.byte1;
               *( p_buffer + offset - 5 ) = rpt.byte2;
               *( p_buffer + offset - 4 ) = pat.byte3;
               *( p_buffer + offset - 3 ) = pat.byte4;

               pat.byte1 = rpt.byte1;
               pat.byte2 = rpt.byte2;
#ifdef DEBUG_ENCODE
cout << "backrepl @" << ( offset - 4 ) << " with: " << pat << endl;
#endif

               if( !meta_patterns.has_pattern( pat ) )
                  meta_patterns.add_pattern( pat, offset - 4 );

               offset -= 2;
               can_continue = true;
               last_pattern_offset = ( offset - 2 );
            }
         }
      }
   }

   return can_continue;
}

void perform_meta_combines( meta_pattern_info& meta_patterns, unsigned char* p_buffer, size_t& end_offset, size_t& last_pattern_offset )
{
   for( size_t i = 0; i < c_max_combines; i++ )
   {
      if( !combine_meta_patterns( meta_patterns, p_buffer, end_offset, last_pattern_offset ) )
         break;
   }
}

bool replace_meta_pattern( meta_pattern_info& meta_patterns,
 unsigned char* p_buffer, size_t offset, unsigned char& new_byte1, unsigned char& new_byte2, size_t& end_offset, size_t& last_pattern_offset )
{
   bool was_replaced = false;

   if( offset >= c_min_pat_length )
   {
      meta_pattern pat;

      pat.byte1 = *( p_buffer + offset );
      pat.byte2 = *( p_buffer + offset + 1 );

      if( ( ( pat.byte1 & c_nibble_one ) != c_nibble_one )
       && pat.byte1 == new_byte1 && pat.byte2 == new_byte2 )
      {
         new_byte1 = 0xf0;
         new_byte2 = 0x00;
      }

      pat.byte3 = new_byte1;
      pat.byte4 = new_byte2;

      // NOTE: If the meta-pattern already exists then replace the last back-ref
      // with the back-ref to the meta-pattern otherwise add a new meta-pattern.
      if( meta_patterns.has_pattern( pat ) )
      {
         was_replaced = true;
         last_pattern_offset = offset;

         size_t old_end_offset = end_offset;
         bool had_prior_pattern = meta_patterns.has_offset( offset - 2 );

         meta_patterns.remove_offsets_from( offset - 2 );

#ifdef DEBUG_ENCODE
cout << "replaced " << pat << " @" << offset << " with: " << meta_patterns[ pat ] << " (for new)" << endl;
#endif
         *( p_buffer + offset ) = meta_patterns[ pat ].first;
         *( p_buffer + offset + 1 ) = meta_patterns[ pat ].second;

         perform_meta_combines( meta_patterns, p_buffer, end_offset, last_pattern_offset );

         if( had_prior_pattern && old_end_offset == end_offset )
         {
            pat.byte1 = *( p_buffer + offset - 2 );
            pat.byte2 = *( p_buffer + offset - 1 );
            pat.byte3 = *( p_buffer + offset );
            pat.byte4 = *( p_buffer + offset + 1 );

            if( !meta_patterns.has_pattern( pat ) )
               meta_patterns.add_pattern( pat, offset - 2 );
         }
#ifdef DEBUG_ENCODE
dump_bytes( "modified ==> ", p_buffer, end_offset );
#endif
      }
      else if( ( pat.byte1 & c_nibble_one ) != c_nibble_one )
      {
         meta_patterns.remove_offsets_from( offset );

         last_pattern_offset = offset;
         meta_patterns.add_pattern( pat, offset );
      }
   }

#ifdef DEBUG_ENCODE
cout << "************************" << endl;
check_meta_patterns( meta_patterns, p_buffer, offset );
cout << "************************" << endl;
#endif
   return was_replaced;
}

bool replace_extra_pattern( map< string, size_t >& extra_patterns, const string& pattern, unsigned char* p_buffer, size_t& output_offset )
{
   bool was_replaced = false;

   if( !extra_patterns.count( pattern ) )
      extra_patterns.insert( make_pair( pattern, output_offset - 2 ) );
   else
   {
      was_replaced = true;

      size_t offset = extra_patterns[ pattern ];

      unsigned char byte1 = c_high_bit_value | ( ( offset & 0x0f00 ) >> 8 );
      byte1 |= ( pattern.length( ) - c_min_pat_length ) << 4;

      unsigned char byte2 = ( offset & 0x00ff );

      bool was_incremented = false;

      if( output_offset > c_min_pat_length
       && *( p_buffer + output_offset - 4 ) == byte1
       && *( p_buffer + output_offset - 3 ) == byte2 )
      {
         byte1 = c_nibble_one;
         byte2 = 0x00;
      }
      else if( output_offset > ( c_min_pat_length * 2 )
       && *( p_buffer + output_offset - 6 ) == byte1
       && *( p_buffer + output_offset - 5 ) == byte2
       && ( ( *( p_buffer + output_offset - 4 ) & c_nibble_one ) == c_nibble_one ) )
      {
         if( *( p_buffer + output_offset - 4 ) != c_max_repeats_hi
          || *( p_buffer + output_offset - 3 ) != c_max_repeats_lo )
         {
            if( *( p_buffer + output_offset - 3 ) != c_max_repeats_lo )
               ++( *( p_buffer + output_offset - 3 ) );
            else
            {
               *( p_buffer + output_offset - 3 ) = 0;
               ++( *( p_buffer + output_offset - 4 ) );
            }

            output_offset -= 2;
            was_incremented = true;
         }
      }

      if( !was_incremented )
      {
         *( p_buffer + output_offset - 2 ) = byte1;
         *( p_buffer + output_offset - 1 ) = byte2;
      }

#ifdef DEBUG_ENCODE
dump_bytes( "extra pattern ==> ", ( unsigned char* )pattern.c_str( ), pattern.length( ) );
#endif
   }

   return was_replaced;
}

// NOTE: Format must be either <pat><rpt> or <pat><pat> with <pat> values being either in the
// form of a simple 7-bit pattern or another meta-pattern which will be expanded recursively.
string expand_meta_pattern( const string& meta, const unsigned char* p_encoded, size_t indent = 0 )
{
   string pattern( meta );

   if( meta.length( ) >= 2 && ( meta[ 0 ] & c_high_bit_value ) )
   {
      unsigned char byte1 = meta[ 0 ];
      unsigned char byte2 = meta[ 1 ];

      size_t pat_length = ( byte1 & 0x70 ) >> 4;
      size_t pat_offset = ( byte1 & c_nibble_two ) << 8;

      pat_length += c_min_pat_length;
      pat_offset += byte2;

      pattern = string( ( const char* )( p_encoded + pat_offset ), pat_length );

      if( pattern[ 0 ] & c_high_bit_value )
      {
#ifdef DEBUG_DECODE
cout << "pattern: ";
if( indent )
cout << string( indent * 6, ' ' );
dump_bytes( "", ( unsigned char* )pattern.c_str( ), pattern.length( ) );
#endif
         if( pattern.length( ) >= c_meta_pat_length - 1 )
         {
            string new_pattern;

            if( ( pattern[ c_meta_pat_length - 2 ] & c_nibble_one ) == c_nibble_one )
            {
               size_t pat_repeats = ( pattern[ c_meta_pat_length - 2 ] & c_nibble_two ) << 8;
               pat_repeats += ( unsigned char )pattern[ c_meta_pat_length - 1 ] + 1;

#ifdef DEBUG_DECODE
cout << "pat repeats = " << pat_repeats << endl;
#endif
               pattern.erase( c_meta_pat_length - 2 );

               for( size_t i = 0; i < pat_repeats + 1; i++ )
                  new_pattern += expand_meta_pattern( pattern, p_encoded, indent + 1 );
            }
            else
            {
               new_pattern = expand_meta_pattern( pattern.substr( 0, c_meta_pat_length - 2 ), p_encoded, indent + 1 );
               new_pattern += expand_meta_pattern( pattern.substr( c_meta_pat_length - 2 ), p_encoded, indent + 1 );
#ifdef DEBUG_DECODE
cout << "new pattern = " << new_pattern << endl;
#endif
            }

            pattern = new_pattern;
         }
      }
   }

   return pattern;
}

}

void decode_clz_data( istream& is, ostream& os )
{
   deque< string > outputs;
   set< size_t > meta_offsets;

   unsigned char input_buffer[ c_max_encoded_chunk_size ];

   while( true )
   {
      size_t bytes_read = expand_input( is, input_buffer, c_max_encoded_chunk_size );

      if( bytes_read == 0 )
         break;

#ifdef DEBUG_DECODE
cout << "bytes read = " << bytes_read << endl;
#endif
      if( bytes_read <= c_min_pat_length )
         os.write( ( const char* )input_buffer, bytes_read );
      else
      {
         size_t offset = 0;

         while( true )
         {
            unsigned char byte = input_buffer[ offset ];

            if( byte & c_high_bit_value )
               meta_offsets.insert( offset++ );

            if( ++offset > bytes_read - 1 )
               break;
         }

#ifdef DEBUG_DECODE
cout << "meta_offsets.size( ) = " <<  meta_offsets.size( ) << endl;
#endif
         if( meta_offsets.empty( ) )
            os.write( ( const char* )input_buffer, bytes_read );
         else
         {
            set< size_t >::iterator si = --meta_offsets.end( );

            size_t num_repeats = 0;
            size_t next_offset = *si;
            size_t last_repeat_offset = 0;

            size_t last_offset = bytes_read;

            // NOTE: Any bytes after the last meta-pair are immediately pushed to the back of the output.
            if( next_offset < bytes_read - 2 )
               outputs.push_back( string( ( const char* )( input_buffer + next_offset + 2 ), last_offset - next_offset - 2 ) );

            while( true )
            {
#ifdef DEBUG_DECODE
cout << "next_offset = " << next_offset << endl;
#endif
               string pat( ( const char* )( input_buffer + next_offset ), 2 );

               unsigned char byte1 = pat[ 0 ];
               unsigned char byte2 = pat[ 1 ];

#ifdef DEBUG_DECODE
dump_bytes( "meta: ", ( unsigned char* )pat.c_str( ), 2 );
#endif
               if( ( byte1 & c_nibble_one ) == c_nibble_one )
               {
                  num_repeats = ( byte1 & c_nibble_two ) << 8;
                  num_repeats += ( byte2 + 1 );
#ifdef DEBUG_DECODE
cout << "meta repeats = " << num_repeats << endl;
#endif
               }
               else
               {
                  string output;
                  pat = expand_meta_pattern( pat, input_buffer );

                  for( size_t i = 0; i < num_repeats + 1; i++ )
                     output += pat;

#ifdef DEBUG_DECODE
cout << "num_repeats = " << num_repeats << ", expanded pat: " << pat << "\n output ==> " << output << endl;
#endif
                  num_repeats = 0;
                  outputs.push_front( output );
               }

               last_offset = next_offset;

               if( si == meta_offsets.begin( ) )
                  break;

               next_offset = *--si;

               if( next_offset < last_offset - 2 )
                  outputs.push_front( string( ( const char* )( input_buffer + next_offset + 2 ), last_offset - next_offset - 2 ) );
            }

            if( last_offset != 0 )
               os.write( ( const char* )input_buffer, last_offset );

            string last_repeat_output;
            bool in_last_repeat = false;

            for( size_t i = 0; i < outputs.size( ); i++ )
               os << outputs[ i ];
         }
      }

      if( bytes_read < c_max_encoded_chunk_size )
         break;
   }
}

void encode_clz_data( istream& is, ostream& os )
{
   size_t num = 0;
   size_t output_offset = 0;
   size_t last_pair_repeats = 0;
   size_t last_pattern_offset = 0;
   size_t last_back_ref_offset = 0;

   size_t max_offset = c_max_offset;
   size_t max_repeats = c_max_repeats;

   meta_pair last_pair;
   repeat_info last_repeat_info;

   meta_pattern_info meta_patterns;
   map< string, size_t > extra_patterns;

   unsigned char input_buffer[ c_max_pat_length + 2 ];
   unsigned char output_buffer[ c_max_encoded_chunk_size ];

   memset( input_buffer, 0, sizeof( input_buffer ) );
   memset( output_buffer, 0, sizeof( output_buffer ) );

   while( true )
   {
      while( num < c_max_pat_length )
      {
         if( is.eof( ) )
            break;

         if( is.read( ( char* )input_buffer + num, 1 ) )
            ++num;

         size_t bytes_from_end = 1;

         if( num && ( input_buffer[ 0 ] & c_high_bit_value ) )
            bytes_from_end += 2;

         if( output_offset + num >= max_offset - bytes_from_end )
            break;
      }

      if( !num )
         break;

#ifdef DEBUG_ENCODE
cout << "(read) num = " << num << ' ';
dump_bytes( "input data = ", input_buffer, num );
#endif
      if( num < c_min_pat_length || output_offset < c_min_pat_length )
      {
         if( last_pair_repeats )
         {
            unsigned char rbyte1 = c_nibble_one | ( ( --last_pair_repeats & 0x0f00 ) >> 8 );
            unsigned char rbyte2 = ( last_pair_repeats & 0x00ff );

            if( !replace_meta_pattern( meta_patterns, output_buffer,
             last_back_ref_offset, rbyte1, rbyte2, output_offset, last_pattern_offset ) )
            {
               output_buffer[ output_offset++ ] = rbyte1;
               output_buffer[ output_offset++ ] = rbyte2;
            }
         }

         last_pair.first = last_pair.second = last_pair_repeats = 0;

         bool was_extra_pattern = false;

         if( num < c_min_pat_length
          && last_pattern_offset == output_offset - 2
          && ( ( input_buffer[ 0 ] & c_high_bit_value ) != c_high_bit_value ) )
         {
            string pattern( ( const char* )&output_buffer[ last_pattern_offset ], 2 );
            pattern += string( ( const char* )input_buffer, num );

            was_extra_pattern = replace_extra_pattern( extra_patterns, pattern, output_buffer, output_offset );
         }

         if( !was_extra_pattern )
         {
            memcpy( output_buffer + output_offset, input_buffer, min( num, c_min_pat_length ) );
            output_offset += min( num, c_min_pat_length );
         }

         // NOTE: If less than the minimum pattern length then it is the last output.
         if( num < c_min_pat_length )
            break;

         memmove( input_buffer, input_buffer + c_min_pat_length, num - c_min_pat_length );
         num -= c_min_pat_length;
      }

#ifdef DEBUG_ENCODE
cout << "num now = " << num << ", output_offset = " << output_offset << endl;
#endif
      size_t start = 0;

      size_t length = 1;
      size_t offset = 0;

      bool input_starts_with_back_ref = ( input_buffer[ 0 ] & c_high_bit_value );

      size_t last_offset_for_pattern = max_offset - 2;

      if( input_starts_with_back_ref )
         last_offset_for_pattern -= 2;

      if( output_offset < last_offset_for_pattern )
      {
         for( ; start <= output_offset - c_min_pat_length; start++ )
         {
            size_t i = 0;

            for( ; i < num; i++ )
            {
               if( output_buffer[ start + i ] != input_buffer[ i ] )
                  break;
               else if( i >= length )
               {
                  length = i + 1;
                  offset = start;
               }

               if( start + i >= output_offset )
                  break;
            }

            if( i == num )
            {
               length = num;
               offset = start;
               break;
            }
         }
      }

      // NOTE: Never output the just first part of a back-ref pair.
      if( length == 1 && input_starts_with_back_ref )
         ++length;

#ifdef DEBUG_ENCODE
cout << "length = " << length << endl;
#endif
      if( length < c_min_pat_length )
      {
         if( !input_starts_with_back_ref )
         {
            if( last_pair_repeats )
            {
               unsigned char rbyte1 = c_nibble_one | ( ( --last_pair_repeats & 0x0f00 ) >> 8 );
               unsigned char rbyte2 = ( last_pair_repeats & 0x00ff );

               if( !replace_meta_pattern( meta_patterns, output_buffer,
                last_back_ref_offset, rbyte1, rbyte2, output_offset, last_pattern_offset ) )
               {
                  output_buffer[ output_offset++ ] = rbyte1;
                  output_buffer[ output_offset++ ] = rbyte2;
               }
            }

            last_pair.first = last_pair.second = last_pair_repeats = 0;
         }
         else
            last_back_ref_offset = output_offset;

         bool was_extra_pattern = false;

         if( length < c_min_pat_length
          && last_pattern_offset == output_offset - 2
          && ( ( input_buffer[ 0 ] & c_high_bit_value ) != c_high_bit_value ) )
         {
            string pattern( ( const char* )&output_buffer[ last_pattern_offset ], 2 );
            pattern += string( ( const char* )input_buffer, length );

            was_extra_pattern = replace_extra_pattern( extra_patterns, pattern, output_buffer, output_offset );
         }

         if( !was_extra_pattern )
         {
            memcpy( output_buffer + output_offset, input_buffer, length );
            output_offset += length;
         }

         if( num > length )
            memmove( input_buffer, input_buffer + length, num - length );

         num -= length;

         perform_meta_combines( meta_patterns, output_buffer, output_offset, last_back_ref_offset );
      }
      else
      {
         unsigned char byte1 = 0;
         unsigned char byte2 = 0;

         byte1 = c_high_bit_value | ( ( offset & 0x0f00 ) >> 8 );
         byte1 |= ( length - c_min_pat_length ) << 4;

         byte2 = ( offset & 0x00ff );

         bool bytes_same_as_last_pair = ( byte1 == last_pair.first && byte2 == last_pair.second );

#ifdef DEBUG_ENCODE
cout << "found pattern: " << hex << setw( 2 ) << setfill( '0' ) << ( int )byte1 << setw( 2 ) << setfill( '0' ) << ( int )byte2 << dec << endl;
#endif
         if( last_pair_repeats && ( !bytes_same_as_last_pair || last_pair_repeats >= max_repeats ) )
         {
            unsigned char rbyte1 = c_nibble_one | ( ( --last_pair_repeats & 0x0f00 ) >> 8 );
            unsigned char rbyte2 = ( last_pair_repeats & 0x00ff );

            if( !replace_meta_pattern( meta_patterns, output_buffer,
             last_back_ref_offset, rbyte1, rbyte2, output_offset, last_pattern_offset ) )
            {
               output_buffer[ output_offset++ ] = rbyte1;
               output_buffer[ output_offset++ ] = rbyte2;
            }

            bytes_same_as_last_pair = false;
            last_pair.first = last_pair.second = last_pair_repeats = 0;
         }

         if( !bytes_same_as_last_pair && output_offset >= max_offset - 1 )
         {
            output_buffer[ output_offset++ ] = byte1;
            output_buffer[ output_offset++ ] = byte2;

            num -= length;
         }
         else
         {
            if( bytes_same_as_last_pair )
               ++last_pair_repeats;

            if( !last_pair_repeats )
            {
               bool was_replaced = false;

               size_t old_output_offset = output_offset;

               // NOTE: One back-ref that immediately follows another is handled as a meta-pattern.
               if( last_back_ref_offset && last_back_ref_offset == output_offset - 2 )
               {
                  was_replaced = replace_meta_pattern( meta_patterns, output_buffer,
                   last_back_ref_offset, byte1, byte2, output_offset, last_pattern_offset );
               }

               if( was_replaced )
               {
                  last_pair.first = last_pair.second = last_pair_repeats = 0;
                  memmove( input_buffer, input_buffer + length, num - length );
               }
               else
               {
                  if( length > 2 && num > length )
                     memmove( input_buffer + 2, input_buffer + length, num - length );

                  // NOTE: Insert the back-reference at the start of the input buffer to support
                  // back-referencing from an existing back-reference (to efficiently handle any
                  // steadily increasing in length repeating patterns).
                  input_buffer[ 0 ] = last_pair.first = byte1;
                  input_buffer[ 1 ] = last_pair.second = byte2;

                  num += 2;
               }
            }
            else if( num > length )
               memmove( input_buffer, input_buffer + length, num - length );

            num -= length;
         }
#ifdef DEBUG_ENCODE
cout << "num now = " << num << ", ";
dump_bytes( "input data = ", input_buffer, num );
#endif
      }

#ifdef DEBUG_ENCODE
dump_bytes( "buffered ==> ", output_buffer, output_offset, last_back_ref_offset );
#endif
      if( output_offset >= max_offset )
         perform_meta_combines( meta_patterns, output_buffer, output_offset, last_back_ref_offset );

      if( output_offset >= max_offset )
      {
         shrink_output( output_buffer, output_offset );
         os.write( ( char* )output_buffer, output_offset + 1 );

         meta_patterns.clear( );
         extra_patterns.clear( );

         last_pair.first = last_pair.second = last_pair_repeats = 0;
         output_offset = last_pattern_offset = last_back_ref_offset = 0;
      }
   }

   if( last_pair_repeats )
   {
      unsigned char rbyte1 = c_nibble_one | ( ( --last_pair_repeats & 0x0f00 ) >> 8 );
      unsigned char rbyte2 = ( last_pair_repeats & 0x00ff );

      if( !replace_meta_pattern( meta_patterns, output_buffer,
       last_back_ref_offset, rbyte1, rbyte2, output_offset, last_pattern_offset ) )
      {
         output_buffer[ output_offset++ ] = rbyte1;
         output_buffer[ output_offset++ ] = rbyte2;
      }
   }

   if( output_offset )
   {
      perform_meta_combines( meta_patterns, output_buffer, output_offset, last_back_ref_offset );

#ifdef DEBUG_ENCODE
cout << "final offset = " << output_offset << endl;
dump_bytes( "buffered ==> ", output_buffer, output_offset );
#endif
      shrink_output( output_buffer, output_offset );
      os.write( ( const char* )output_buffer, output_offset );
   }
}

#ifdef COMPILE_TESTBED_MAIN
int main( int argc, char* argv[ ] )
{
   string arg_1;

   if( argc > 1 )
      arg_1 = string( argv[ 1 ] );

   if( arg_1.empty( ) || arg_1 == "encode" )
   {
      ifstream is( "clz.in", ios::in | ios::binary );
      ofstream os( "clz.out", ios::out | ios::binary );

      encode_clz_data( is, os );
   }

   if( arg_1.empty( ) || arg_1 == "decode" )
   {
      ifstream is( "clz.out", ios::in | ios::binary );
      decode_clz_data( is, cout );
   }
}
#endif