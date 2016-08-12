// ======================================================================== //
// Copyright 2009-2016 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#pragma once

#include "heuristic_binning.h"

namespace embree
{
  namespace isa
  { 


    /*! Performs standard object binning */
#if defined(__AVX512F__)
    template<typename PrimRef, size_t BINS = 16>
#else
      template<typename PrimRef, size_t BINS = 32>
#endif
      struct HeuristicArraySpatialSAH
      {
        typedef BinSplit<BINS> Split;
        typedef BinInfo<BINS,PrimRef> Binner;
        typedef extended_range<size_t> Set;

#if defined(__AVX512F__)
        static const size_t PARALLEL_THRESHOLD = 3*1024; 
        static const size_t PARALLEL_FIND_BLOCK_SIZE = 768;
        static const size_t PARALLEL_PARITION_BLOCK_SIZE = 128;
#else
        static const size_t PARALLEL_THRESHOLD = 3*1024;
        static const size_t PARALLEL_FIND_BLOCK_SIZE = 1024;
        static const size_t PARALLEL_PARITION_BLOCK_SIZE = 128;
#endif
        __forceinline HeuristicArraySpatialSAH ()
          : prims0(nullptr) {}
        
        /*! remember prim array */
        __forceinline HeuristicArraySpatialSAH (PrimRef* prims0)
          : prims0(prims0) {}


        /*! finds the best split */
        const Split find(const Set& set, const PrimInfo& pinfo, const size_t logBlockSize)
        {
          //std::cout << std::endl;
          //PING;
          //PRINT(pinfo);
          //PRINT(pinfo.index);
          //if (likely(pinfo.size() < PARALLEL_THRESHOLD)) 
          return sequential_find(set,pinfo,logBlockSize);
          //else
          //  return   parallel_find(set,pinfo,logBlockSize);
        }
        
        /*! finds the best split */
        const Split sequential_find(const Set& set, const PrimInfo& pinfo, const size_t logBlockSize)
        {
          PrimRef* const source = prims0;

          for (size_t i=set.begin();i<set.end();i++)
            assert(subset(source[i].bounds(),pinfo.geomBounds));

          Binner binner(empty); // FIXME: this clear can be optimized away
          const BinMapping<BINS> mapping(pinfo);
          binner.bin(source,set.begin(),set.end(),mapping);
          Split s = binner.best(mapping,logBlockSize);
          s.lcount = binner.getLeftCount(mapping,s);
          return s;
        }
        
        /*! finds the best split */
        __noinline const Split parallel_find(const Set& set, const PrimInfo& pinfo, const size_t logBlockSize)
        {
          Binner binner(empty);
          const BinMapping<BINS> mapping(pinfo);
          const BinMapping<BINS>& _mapping = mapping; // CLANG 3.4 parser bug workaround
          binner = parallel_reduce(set.begin(),set.end(),PARALLEL_FIND_BLOCK_SIZE,binner,
                                   [&] (const range<size_t>& r) -> Binner { Binner binner(empty); binner.bin(prims0+r.begin(),r.size(),_mapping); return binner; },
                                   [&] (const Binner& b0, const Binner& b1) -> Binner { Binner r = b0; r.merge(b1,_mapping.size()); return r; });
          return binner.best(mapping,logBlockSize);
        }
        
        /*! array partitioning */
        /* void split(const Split& spliti, const PrimInfo& pinfo, PrimInfo& left, PrimInfo& right)  */
        /* { */
        /*   Set lset,rset; */
        /*   Set set(pinfo.begin,pinfo.end); */
        /*   if (likely(pinfo.size() < PARALLEL_THRESHOLD))  */
        /*     sequential_split(spliti,set,left,lset,right,rset); */
        /*   else */
        /*     parallel_split(spliti,set,left,lset,right,rset); */
        /* } */
        
        /*! array partitioning */
        void split(const Split& split, const PrimInfo& pinfo, const Set& set, PrimInfo& left, Set& lset, PrimInfo& right, Set& rset) 
        {
          //std::cout << std::endl;
          //if (likely(pinfo.size() < PARALLEL_THRESHOLD)) 
          //PING;
          sequential_split(split,set,left,lset,right,rset);
          //PRINT(split);
          //PRINT(pinfo);
          //PRINT(left);
          //PRINT(right);

          //else                                
          //parallel_split(split,set,left,lset,right,rset);
        }

        /*! array partitioning */
        void sequential_split(const Split& split, const Set& set, PrimInfo& left, Set& lset, PrimInfo& right, Set& rset) 
        {
          // determine input and output primref arrays
          PrimRef* const source = prims0;

          if (unlikely(!split.valid())) {
            deterministic_order(set);
            return splitFallback(set,left,lset,right,rset);
          }
          
          //const size_t countBinningLeft = 

          const size_t begin = set.begin();
          const size_t end   = set.end();
          CentGeomBBox3fa local_left(empty);
          CentGeomBBox3fa local_right(empty);
          const unsigned int splitPos = split.pos;
          const unsigned int splitDim = split.dim;
          const unsigned int splitDimMask = (unsigned int)1 << splitDim; 

#if defined(__AVX512F__)
          const vint16 vSplitPos(splitPos);
          const vbool16 vSplitMask( splitDimMask );
#else
          const vint4 vSplitPos(splitPos);
          const vbool4 vSplitMask( (int)splitDimMask );
#endif

          size_t center = serial_partitioning(source,
                                              begin,end,local_left,local_right,
                                              [&] (const PrimRef& ref) { 
#if defined(__AVX512F__)
                                                return split.mapping.bin_unsafe(ref,vSplitPos,vSplitMask);                                                 
#else
                                                return any(((vint4)split.mapping.bin_unsafe(center2(ref.bounds())) < vSplitPos) & vSplitMask); 
#endif
                                              },
                                              [] (CentGeomBBox3fa& pinfo,const PrimRef& ref) { pinfo.extend(ref.bounds()); });          
          
          assert(center == begin + split.lcount);
          new (&left ) PrimInfo(begin,center,local_left.geomBounds,local_left.centBounds);
          new (&right) PrimInfo(center,end,local_right.geomBounds,local_right.centBounds);
          new (&lset) range<size_t>(begin,center);
          new (&rset) range<size_t>(center,end);
          assert(area(left.geomBounds) >= 0.0f);
          assert(area(right.geomBounds) >= 0.0f);

          for (size_t i=begin;i<center;i++)
            assert(subset(source[i].bounds(),local_left.geomBounds));
          for (size_t i=center;i<end;i++)
            assert(subset(source[i].bounds(),local_right.geomBounds));
        }
        
#if 0
          /*! array partitioning */
          __noinline void parallel_split(const Split& split, const Set& set, PrimInfo& left, Set& lset, PrimInfo& right, Set& rset)
          {
          PrimRef* const source = prims0;

            if (!split.valid()) {
              deterministic_order(set);
              return splitFallback(set,left,lset,right,rset);
            }
          
            const size_t begin = set.begin();
            const size_t end   = set.end();
            left.reset(); 
            right.reset();
            PrimInfo init; init.reset();
            const unsigned int splitPos = split.pos;
            const unsigned int splitDim = split.dim;
            const unsigned int splitDimMask = (unsigned int)1 << splitDim;

#if defined(__AVX512F__)
            const vint16 vSplitPos(splitPos);
            const vbool16 vSplitMask( (int)splitDimMask );
            auto isLeft = [&] (const PrimRef &ref) { return split.mapping.bin_unsafe(ref,vSplitPos,vSplitMask); };
#else
            const vint4 vSplitPos(splitPos);
            const vbool4 vSplitMask( (int)splitDimMask );
            auto isLeft = [&] (const PrimRef &ref) { return any(((vint4)split.mapping.bin_unsafe(center2(ref.bounds())) < vSplitPos) & vSplitMask); };

#endif
            const size_t mid = parallel_in_place_partitioning_static<PARALLEL_PARITION_BLOCK_SIZE,PrimRef,PrimInfo>(
              &prims0[begin],end-begin,init,left,right,isLeft,
              [] (PrimInfo &pinfo,const PrimRef &ref) { pinfo.add(ref.bounds()); },
              [] (PrimInfo &pinfo0,const PrimInfo &pinfo1) { pinfo0.merge(pinfo1); });
          
            const size_t center = begin+mid;
            left.begin  = begin;  left.end  = center; 
            right.begin = center; right.end = end;
          
            new (&lset) range<size_t>(begin,center);
            new (&rset) range<size_t>(center,end);
          }
#endif

          void deterministic_order(const Set& set) 
          {
            /* required as parallel partition destroys original primitive order */
            std::sort(&prims0[set.begin()],&prims0[set.end()]);
          }

          void splitFallback(const Set& set, 
                             PrimInfo& linfo, Set& lset, 
                             PrimInfo& rinfo, Set& rset)
          {
            const size_t begin = set.begin();
            const size_t end   = set.end();
            const size_t center = (begin + end)/2;

            PrimRef* const source = prims0;
          
            CentGeomBBox3fa left; left.reset();
            for (size_t i=begin; i<center; i++)
            {
              left.extend(source[i].bounds());
            }
            new (&linfo) PrimInfo(begin,center,left.geomBounds,left.centBounds);
          
            CentGeomBBox3fa right; right.reset();
            for (size_t i=center; i<end; i++)
            {
              right.extend(source[i].bounds());	
            }
            new (&rinfo) PrimInfo(center,end,right.geomBounds,right.centBounds);         
            new (&lset) range<size_t>(begin,center);
            new (&rset) range<size_t>(center,end);
          }
        
        private:
          PrimRef* const prims0;
        };
      }
  }