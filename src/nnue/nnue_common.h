/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2020 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Constants used in NNUE evaluation function

#ifndef NNUE_COMMON_H_INCLUDED
#define NNUE_COMMON_H_INCLUDED

#if defined(USE_AVX2)
#include <immintrin.h>

#elif defined(USE_SSE41)
#include <smmintrin.h>

#elif defined(USE_SSSE3)
#include <tmmintrin.h>

#elif defined(USE_SSE2)
#include <emmintrin.h>
#endif

namespace Eval::NNUE {

  // Version of the evaluation file
  constexpr std::uint32_t kVersion = 0x7AF32F16u;

  // Constant used in evaluation value calculation
  constexpr int FV_SCALE = 16;
  constexpr int kWeightScaleBits = 6;

  // Size of cache line (in bytes)
  constexpr std::size_t kCacheLineSize = 64;

  // SIMD width (in bytes)
  #if defined(USE_AVX2)
  constexpr std::size_t kSimdWidth = 32;

  #elif defined(USE_SSE2)
  constexpr std::size_t kSimdWidth = 16;

  #elif defined(IS_ARM)
  constexpr std::size_t kSimdWidth = 16;
  #endif

  constexpr std::size_t kMaxSimdWidth = 32;

  // Type of input feature after conversion
  using TransformedFeatureType = std::uint8_t;
  using IndexType = std::uint32_t;

  // Round n up to be a multiple of base
  template <typename IntType>
  constexpr IntType CeilToMultiple(IntType n, IntType base) {
    return (n + base - 1) / base * base;
  }

}  // namespace Eval::NNUE

#endif // #ifndef NNUE_COMMON_H_INCLUDED
