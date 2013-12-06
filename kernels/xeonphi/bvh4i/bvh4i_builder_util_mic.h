// ======================================================================== //
// Copyright 2009-2013 Intel Corporation                                    //
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

#ifndef BVH4I_BUILDER_UTIL_MIC_H
#define BVH4I_BUILDER_UTIL_MIC_H

#include "bvh4i.h"
#include "builders/primref.h"
#include "geometry/triangle1.h"

namespace embree
{
  
#define L1_PREFETCH_ITEMS 2
#define L2_PREFETCH_ITEMS 16

  __align(64) int identity[16]         = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };
  __align(64) int reverse_identity[16] = { 15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0 };

  __forceinline mic_f reverse(const mic_f &a) 
  {
    return _mm512_permutev_ps(load16i(reverse_identity),a);
  }
  
  __forceinline mic_f prefix_area_rl(const mic_f min_x,
				     const mic_f min_y,
				     const mic_f min_z,
				     const mic_f max_x,
				     const mic_f max_y,
				     const mic_f max_z)
  {
    const mic_f r_min_x = prefix_min(reverse(min_x));
    const mic_f r_min_y = prefix_min(reverse(min_y));
    const mic_f r_min_z = prefix_min(reverse(min_z));
    const mic_f r_max_x = prefix_max(reverse(max_x));
    const mic_f r_max_y = prefix_max(reverse(max_y));
    const mic_f r_max_z = prefix_max(reverse(max_z));
    
    const mic_f dx = r_max_x - r_min_x;
    const mic_f dy = r_max_y - r_min_y;
    const mic_f dz = r_max_z - r_min_z;
    
    const mic_f area_rl = (dx*dy+dx*dz+dy*dz) * mic_f(2.0f);
    return reverse(shl1_zero_extend(area_rl));
  }

  __forceinline mic_f prefix_area_lr(const mic_f min_x,
				     const mic_f min_y,
				     const mic_f min_z,
				     const mic_f max_x,
				     const mic_f max_y,
				     const mic_f max_z)
  {
    const mic_f r_min_x = prefix_min(min_x);
    const mic_f r_min_y = prefix_min(min_y);
    const mic_f r_min_z = prefix_min(min_z);
    const mic_f r_max_x = prefix_max(max_x);
    const mic_f r_max_y = prefix_max(max_y);
    const mic_f r_max_z = prefix_max(max_z);
    
    const mic_f dx = r_max_x - r_min_x;
    const mic_f dy = r_max_y - r_min_y;
    const mic_f dz = r_max_z - r_min_z;
  
    const mic_f area_lr = (dx*dy+dx*dz+dy*dz) * mic_f(2.0f);
    return area_lr;
  }


  __forceinline mic_i prefix_count(const mic_i c)
  {
    return prefix_sum(c);
  }

  __forceinline void fastbin(const PrimRef * __restrict__ const aabb,
			     const unsigned int start,
			     const unsigned int end,
			     const mic_f &centroidBoundsMin_2,
			     const mic_f &scale,
			     mic_f lArea[3],
			     mic_f rArea[3],
			     mic_i lNum[3])
  {
    const PrimRef * __restrict__ aptr = aabb + start;

    prefetch<PFHINT_NT>(aptr);
    prefetch<PFHINT_L2>(aptr+2);
    prefetch<PFHINT_L2>(aptr+4);
    prefetch<PFHINT_L2>(aptr+6);
    prefetch<PFHINT_L2>(aptr+8);
    prefetch<PFHINT_L2>(aptr+10);

    const mic_f init_min = mic_f::inf();
    const mic_f init_max = mic_f::minus_inf();
    const mic_i zero     = mic_i::zero();

    mic_f min_x0,min_x1,min_x2;
    mic_f min_y0,min_y1,min_y2;
    mic_f min_z0,min_z1,min_z2;
    mic_f max_x0,max_x1,max_x2;
    mic_f max_y0,max_y1,max_y2;
    mic_f max_z0,max_z1,max_z2;
    mic_i count0,count1,count2;

    min_x0 = init_min;
    min_x1 = init_min;
    min_x2 = init_min;
    min_y0 = init_min;
    min_y1 = init_min;
    min_y2 = init_min;
    min_z0 = init_min;
    min_z1 = init_min;
    min_z2 = init_min;

    max_x0 = init_max;
    max_x1 = init_max;
    max_x2 = init_max;
    max_y0 = init_max;
    max_y1 = init_max;
    max_y2 = init_max;
    max_z0 = init_max;
    max_z1 = init_max;
    max_z2 = init_max;

    count0 = zero;
    count1 = zero;
    count2 = zero;

    for (unsigned int j = start;j < end;j++,aptr++)
      {
	prefetch<PFHINT_NT>(aptr+2);
	prefetch<PFHINT_L2>(aptr+16);

	const mic_f b_min = broadcast4to16f((float*)&aptr->lower);
	const mic_f b_max = broadcast4to16f((float*)&aptr->upper);    

	const mic_f centroid_2 = b_min + b_max;
	const mic_i binID = mic_i((centroid_2 - centroidBoundsMin_2)*scale);

	assert(0 <= binID[0] && binID[0] < 16);
	assert(0 <= binID[1] && binID[1] < 16);
	assert(0 <= binID[2] && binID[2] < 16);

	const mic_i id = load16i(identity);
	const mic_m m_update_x = eq(id,swAAAA(binID));
	const mic_m m_update_y = eq(id,swBBBB(binID));
	const mic_m m_update_z = eq(id,swCCCC(binID));

	min_x0 = mask_min(m_update_x,min_x0,min_x0,swAAAA(b_min));
	min_y0 = mask_min(m_update_x,min_y0,min_y0,swBBBB(b_min));
	min_z0 = mask_min(m_update_x,min_z0,min_z0,swCCCC(b_min));
	// ------------------------------------------------------------------------      
	max_x0 = mask_max(m_update_x,max_x0,max_x0,swAAAA(b_max));
	max_y0 = mask_max(m_update_x,max_y0,max_y0,swBBBB(b_max));
	max_z0 = mask_max(m_update_x,max_z0,max_z0,swCCCC(b_max));
	// ------------------------------------------------------------------------
	min_x1 = mask_min(m_update_y,min_x1,min_x1,swAAAA(b_min));
	min_y1 = mask_min(m_update_y,min_y1,min_y1,swBBBB(b_min));
	min_z1 = mask_min(m_update_y,min_z1,min_z1,swCCCC(b_min));      
	// ------------------------------------------------------------------------      
	max_x1 = mask_max(m_update_y,max_x1,max_x1,swAAAA(b_max));
	max_y1 = mask_max(m_update_y,max_y1,max_y1,swBBBB(b_max));
	max_z1 = mask_max(m_update_y,max_z1,max_z1,swCCCC(b_max));
	// ------------------------------------------------------------------------
	min_x2 = mask_min(m_update_z,min_x2,min_x2,swAAAA(b_min));
	min_y2 = mask_min(m_update_z,min_y2,min_y2,swBBBB(b_min));
	min_z2 = mask_min(m_update_z,min_z2,min_z2,swCCCC(b_min));
	// ------------------------------------------------------------------------      
	max_x2 = mask_max(m_update_z,max_x2,max_x2,swAAAA(b_max));
	max_y2 = mask_max(m_update_z,max_y2,max_y2,swBBBB(b_max));
	max_z2 = mask_max(m_update_z,max_z2,max_z2,swCCCC(b_max));
	// ------------------------------------------------------------------------
	count0 = mask_add(m_update_x,count0,count0,mic_i::one());
	count1 = mask_add(m_update_y,count1,count1,mic_i::one());
	count2 = mask_add(m_update_z,count2,count2,mic_i::one());      
	//evictL1(aptr-2);
      }
    prefetch<PFHINT_L1EX>(&rArea[0]);
    prefetch<PFHINT_L1EX>(&lArea[0]);
    prefetch<PFHINT_L1EX>(&lNum[0]);
    rArea[0] = prefix_area_rl(min_x0,min_y0,min_z0,max_x0,max_y0,max_z0);
    lArea[0] = prefix_area_lr(min_x0,min_y0,min_z0,max_x0,max_y0,max_z0);
    lNum[0]  = prefix_count(count0);

    prefetch<PFHINT_L1EX>(&rArea[1]);
    prefetch<PFHINT_L1EX>(&lArea[1]);
    prefetch<PFHINT_L1EX>(&lNum[1]);
    rArea[1] = prefix_area_rl(min_x1,min_y1,min_z1,max_x1,max_y1,max_z1);
    lArea[1] = prefix_area_lr(min_x1,min_y1,min_z1,max_x1,max_y1,max_z1);
    lNum[1]  = prefix_count(count1);

    prefetch<PFHINT_L1EX>(&rArea[2]);
    prefetch<PFHINT_L1EX>(&lArea[2]);
    prefetch<PFHINT_L1EX>(&lNum[2]);
    rArea[2] = prefix_area_rl(min_x2,min_y2,min_z2,max_x2,max_y2,max_z2);
    lArea[2] = prefix_area_lr(min_x2,min_y2,min_z2,max_x2,max_y2,max_z2);
    lNum[2]  = prefix_count(count2);
  }


  class __align(64) Bin16
  {
  public:
    mic_f min_x[3];
    mic_f min_y[3];
    mic_f min_z[3];
    mic_f max_x[3];
    mic_f max_y[3];
    mic_f max_z[3];
    mic_i count[3];
    mic_i thread_count[3];

    Bin16() {}

    __forceinline void prefetchL2()
    {
      const unsigned int size = sizeof(Bin16);
#pragma unroll(size/64)
      for (size_t i=0;i<size;i+=64)
	prefetch<PFHINT_L2>(((char*)this) + i);
	
    }
    __forceinline void prefetchL2EX()
    {
      prefetch<PFHINT_L2EX>(&min_x[0]);
      prefetch<PFHINT_L2EX>(&min_x[1]);
      prefetch<PFHINT_L2EX>(&min_x[2]);

      prefetch<PFHINT_L2EX>(&min_y[0]);
      prefetch<PFHINT_L2EX>(&min_y[1]);
      prefetch<PFHINT_L2EX>(&min_y[2]);

      prefetch<PFHINT_L2EX>(&min_z[0]);
      prefetch<PFHINT_L2EX>(&min_z[1]);
      prefetch<PFHINT_L2EX>(&min_z[2]);

      prefetch<PFHINT_L2EX>(&max_x[0]);
      prefetch<PFHINT_L2EX>(&max_x[1]);
      prefetch<PFHINT_L2EX>(&max_x[2]);

      prefetch<PFHINT_L2EX>(&max_y[0]);
      prefetch<PFHINT_L2EX>(&max_y[1]);
      prefetch<PFHINT_L2EX>(&max_y[2]);

      prefetch<PFHINT_L2EX>(&max_z[0]);
      prefetch<PFHINT_L2EX>(&max_z[1]);
      prefetch<PFHINT_L2EX>(&max_z[2]);

      prefetch<PFHINT_L2EX>(&count[0]);
      prefetch<PFHINT_L2EX>(&count[1]);
      prefetch<PFHINT_L2EX>(&count[2]);

      prefetch<PFHINT_L2EX>(&thread_count[0]);
      prefetch<PFHINT_L2EX>(&thread_count[1]);
      prefetch<PFHINT_L2EX>(&thread_count[2]);
    }


    __forceinline void reset()
    {
      const mic_f init_min = mic_f::inf();
      const mic_f init_max = mic_f::minus_inf();
      const mic_i zero     = mic_i::zero();

      min_x[0] = init_min;
      min_x[1] = init_min;
      min_x[2] = init_min;

      min_y[0] = init_min;
      min_y[1] = init_min;
      min_y[2] = init_min;

      min_z[0] = init_min;
      min_z[1] = init_min;
      min_z[2] = init_min;

      max_x[0] = init_max;
      max_x[1] = init_max;
      max_x[2] = init_max;

      max_y[0] = init_max;
      max_y[1] = init_max;
      max_y[2] = init_max;

      max_z[0] = init_max;
      max_z[1] = init_max;
      max_z[2] = init_max;

      count[0] = zero;
      count[1] = zero;
      count[2] = zero;
    }


    __forceinline void merge(const Bin16& b)
    {
#pragma unroll(3)
      for (unsigned int i=0;i<3;i++)
	{
	  min_x[i] = min(min_x[i],b.min_x[i]);
	  min_y[i] = min(min_y[i],b.min_y[i]);
	  min_z[i] = min(min_z[i],b.min_z[i]);

	  max_x[i] = max(max_x[i],b.max_x[i]);
	  max_y[i] = max(max_y[i],b.max_y[i]);
	  max_z[i] = max(max_z[i],b.max_z[i]);

	  count[i] += b.count[i];
	}

    } 

  };

  __forceinline bool operator==(const Bin16 &a, const Bin16 &b) { 
    mic_m mask = 0xffff;
#pragma unroll(3)
    for (unsigned int i=0;i<3;i++)
      {
	mask &= eq(a.min_x[i],b.min_x[i]);
	mask &= eq(a.min_y[i],b.min_y[i]);
	mask &= eq(a.min_z[i],b.min_z[i]);

	mask &= eq(a.max_x[i],b.max_x[i]);
	mask &= eq(a.max_y[i],b.max_y[i]);
	mask &= eq(a.max_z[i],b.max_z[i]);

	mask &= eq(a.count[i],b.count[i]);
      }
    return mask == (mic_m)0xffff;
  };

  __forceinline bool operator!=(const Bin16 &a, const Bin16 &b) { 
    return !(a==b);
  }

  __forceinline std::ostream &operator<<(std::ostream &o, const Bin16 &v)
  {
#pragma unroll(3)
    for (unsigned int i=0;i<3;i++)
      {
	DBG_PRINT(v.min_x[i]);
	DBG_PRINT(v.min_y[i]);
	DBG_PRINT(v.min_z[i]);

	DBG_PRINT(v.max_x[i]);
	DBG_PRINT(v.max_y[i]);
	DBG_PRINT(v.max_z[i]);

	DBG_PRINT(v.count[i]);
      }

    return o;
  }



  __forceinline void fastbin_copy(const PrimRef * __restrict__ const aabb,
				  PrimRef * __restrict__ const tmp_aabb,
				  const unsigned int start,
				  const unsigned int end,
				  const mic_f &centroidBoundsMin_2,
				  const mic_f &scale,
				  Bin16 &bin16)
  {
    const PrimRef * __restrict__ aptr = aabb + start;

    prefetch<PFHINT_NT>(aptr);
    prefetch<PFHINT_L2>(aptr+2);
    prefetch<PFHINT_L2>(aptr+4);
    prefetch<PFHINT_L2>(aptr+6);
    prefetch<PFHINT_L2>(aptr+8);

    PrimRef * __restrict__ tmp_aptr   = tmp_aabb + start;

    prefetch<PFHINT_NTEX>(aptr);
    prefetch<PFHINT_L2EX>(aptr+2);
    prefetch<PFHINT_L2EX>(aptr+4);
    prefetch<PFHINT_L2EX>(aptr+6);
    prefetch<PFHINT_L2EX>(aptr+8);

    const mic_f init_min = mic_f::inf();
    const mic_f init_max = mic_f::minus_inf();
    const mic_i zero     = mic_i::zero();

    mic_f min_x0,min_x1,min_x2;
    mic_f min_y0,min_y1,min_y2;
    mic_f min_z0,min_z1,min_z2;
    mic_f max_x0,max_x1,max_x2;
    mic_f max_y0,max_y1,max_y2;
    mic_f max_z0,max_z1,max_z2;
    mic_i count0,count1,count2;

    min_x0 = init_min;
    min_x1 = init_min;
    min_x2 = init_min;
    min_y0 = init_min;
    min_y1 = init_min;
    min_y2 = init_min;
    min_z0 = init_min;
    min_z1 = init_min;
    min_z2 = init_min;

    max_x0 = init_max;
    max_x1 = init_max;
    max_x2 = init_max;
    max_y0 = init_max;
    max_y1 = init_max;
    max_y2 = init_max;
    max_z0 = init_max;
    max_z1 = init_max;
    max_z2 = init_max;

    count0 = zero;
    count1 = zero;
    count2 = zero;

    for (unsigned int j = start;j < end;j++,aptr++,tmp_aptr++)
      {
	prefetch<PFHINT_NT>(aptr+2);
	prefetch<PFHINT_L2>(aptr+8);

	prefetch<PFHINT_NTEX>(tmp_aptr+2);
	prefetch<PFHINT_L2EX>(tmp_aptr+8);


	const mic_f b_min = broadcast4to16f((float*)&aptr->lower);
	const mic_f b_max = broadcast4to16f((float*)&aptr->upper);    

	const mic_f centroid_2 = b_min + b_max; // FIXME: use sub + upconv?
	const mic_i binID = mic_i((centroid_2 - centroidBoundsMin_2)*scale);

	assert(0 <= binID[0] && binID[0] < 16);
	assert(0 <= binID[1] && binID[1] < 16);
	assert(0 <= binID[2] && binID[2] < 16);

	const mic_i id = load16i(identity);
	const mic_m m_update_x = eq(id,swAAAA(binID));
	const mic_m m_update_y = eq(id,swBBBB(binID));
	const mic_m m_update_z = eq(id,swCCCC(binID));

	min_x0 = mask_min(m_update_x,min_x0,min_x0,swAAAA(b_min));
	min_y0 = mask_min(m_update_x,min_y0,min_y0,swBBBB(b_min));
	min_z0 = mask_min(m_update_x,min_z0,min_z0,swCCCC(b_min));
	// ------------------------------------------------------------------------      
	max_x0 = mask_max(m_update_x,max_x0,max_x0,swAAAA(b_max));
	max_y0 = mask_max(m_update_x,max_y0,max_y0,swBBBB(b_max));
	max_z0 = mask_max(m_update_x,max_z0,max_z0,swCCCC(b_max));
	// ------------------------------------------------------------------------
	min_x1 = mask_min(m_update_y,min_x1,min_x1,swAAAA(b_min));
	min_y1 = mask_min(m_update_y,min_y1,min_y1,swBBBB(b_min));
	min_z1 = mask_min(m_update_y,min_z1,min_z1,swCCCC(b_min));      
	// ------------------------------------------------------------------------      
	max_x1 = mask_max(m_update_y,max_x1,max_x1,swAAAA(b_max));
	max_y1 = mask_max(m_update_y,max_y1,max_y1,swBBBB(b_max));
	max_z1 = mask_max(m_update_y,max_z1,max_z1,swCCCC(b_max));
	// ------------------------------------------------------------------------
	min_x2 = mask_min(m_update_z,min_x2,min_x2,swAAAA(b_min));
	min_y2 = mask_min(m_update_z,min_y2,min_y2,swBBBB(b_min));
	min_z2 = mask_min(m_update_z,min_z2,min_z2,swCCCC(b_min));
	// ------------------------------------------------------------------------      
	max_x2 = mask_max(m_update_z,max_x2,max_x2,swAAAA(b_max));
	max_y2 = mask_max(m_update_z,max_y2,max_y2,swBBBB(b_max));
	max_z2 = mask_max(m_update_z,max_z2,max_z2,swCCCC(b_max));
	// ------------------------------------------------------------------------
	count0 = mask_add(m_update_x,count0,count0,mic_i::one());
	count1 = mask_add(m_update_y,count1,count1,mic_i::one());
	count2 = mask_add(m_update_z,count2,count2,mic_i::one());      
	//evictL1(aptr-2);

	*tmp_aptr = *aptr; // FIXME: NGO!
      }

    bin16.prefetchL2EX();

    bin16.min_x[0] = min_x0;
    bin16.min_y[0] = min_y0;
    bin16.min_z[0] = min_z0;
    bin16.max_x[0] = max_x0;
    bin16.max_y[0] = max_y0;
    bin16.max_z[0] = max_z0;

    bin16.min_x[1] = min_x1;
    bin16.min_y[1] = min_y1;
    bin16.min_z[1] = min_z1;
    bin16.max_x[1] = max_x1;
    bin16.max_y[1] = max_y1;
    bin16.max_z[1] = max_z1;

    bin16.min_x[2] = min_x2;
    bin16.min_y[2] = min_y2;
    bin16.min_z[2] = min_z2;
    bin16.max_x[2] = max_x2;
    bin16.max_y[2] = max_y2;
    bin16.max_z[2] = max_z2;

    bin16.count[0] = count0;
    bin16.count[1] = count1;
    bin16.count[2] = count2;

    bin16.thread_count[0] = count0; 
    bin16.thread_count[1] = count1; 
    bin16.thread_count[2] = count2;     
  }


  static __forceinline mic_m lt_split(const PrimRef *__restrict__ const aabb,
				      const unsigned int dim,
				      const mic_f &c,
				      const mic_f &s,
				      const mic_f &bestSplit_f)
  {
    const mic_f b_min = mic_f(aabb->lower[dim]);
    const mic_f b_max = mic_f(aabb->upper[dim]);
    prefetch<PFHINT_NT>(aabb + 2);
    const mic_f centroid_2 = b_min + b_max;
    const mic_f binID = (centroid_2 - c)*s;
    return lt(binID,bestSplit_f);    
  }


  static __forceinline mic_m ge_split(const PrimRef *__restrict__ const aabb,
				      const unsigned int dim,
				      const mic_f &c,
				      const mic_f &s,
				      const mic_f &bestSplit_f)
  {
    const mic_f b_min = mic_f(aabb->lower[dim]);
    const mic_f b_max = mic_f(aabb->upper[dim]);
    prefetch<PFHINT_NT>(aabb-2);
    const mic_f centroid_2 = b_min + b_max;
    const mic_f binID = (centroid_2 - c)*s;
    return ge(binID,bestSplit_f);    
  }

  template<unsigned int DISTANCE>
    __forceinline unsigned int partitionPrimRefs(PrimRef *__restrict__ aabb,
						 const unsigned int begin,
						 const unsigned int end,
						 const unsigned int bestSplit,
						 const unsigned int bestSplitDim,
						 const mic_f &centroidBoundsMin_2,
						 const mic_f &scale,
						 Centroid_Scene_AABB & local_left,
						 Centroid_Scene_AABB & local_right)
    {
      assert(begin <= end);

      PrimRef *__restrict__ l = aabb + begin;
      PrimRef *__restrict__ r = aabb + end;

      const mic_f c = mic_f(centroidBoundsMin_2[bestSplitDim]);
      const mic_f s = mic_f(scale[bestSplitDim]);

      mic_f left_centroidMinAABB = broadcast4to16f(&local_left.centroid2.lower);
      mic_f left_centroidMaxAABB = broadcast4to16f(&local_left.centroid2.upper);
      mic_f left_sceneMinAABB    = broadcast4to16f(&local_left.geometry.lower);
      mic_f left_sceneMaxAABB    = broadcast4to16f(&local_left.geometry.upper);

      mic_f right_centroidMinAABB = broadcast4to16f(&local_right.centroid2.lower);
      mic_f right_centroidMaxAABB = broadcast4to16f(&local_right.centroid2.upper);
      mic_f right_sceneMinAABB    = broadcast4to16f(&local_right.geometry.lower);
      mic_f right_sceneMaxAABB    = broadcast4to16f(&local_right.geometry.upper);

      const mic_f bestSplit_f = mic_f(bestSplit);
      while(1)
	{
	  while (likely(l < r && lt_split(l,bestSplitDim,c,s,bestSplit_f))) 
	    {
	      prefetch<PFHINT_L2EX>(l + DISTANCE);	  
	      {
		const mic_f b_min = broadcast4to16f((float*)&l->lower);
		const mic_f b_max = broadcast4to16f((float*)&l->upper);
		const mic_f centroid2 = b_min+b_max;
		left_centroidMinAABB = min(left_centroidMinAABB,centroid2);
		left_centroidMaxAABB = max(left_centroidMaxAABB,centroid2);
		left_sceneMinAABB    = min(left_sceneMinAABB,b_min);
		left_sceneMaxAABB    = max(left_sceneMaxAABB,b_max);
	      }
	      //evictL1(l-2);

	      ++l;
	    }
	  while (likely(l < r && ge_split(r,bestSplitDim,c,s,bestSplit_f))) 
	    {
	      prefetch<PFHINT_L2EX>(r - DISTANCE);
	      {
		const mic_f b_min = broadcast4to16f((float*)&r->lower);
		const mic_f b_max = broadcast4to16f((float*)&r->upper);
		const mic_f centroid2 = b_min+b_max;
		right_centroidMinAABB = min(right_centroidMinAABB,centroid2);
		right_centroidMaxAABB = max(right_centroidMaxAABB,centroid2);
		right_sceneMinAABB    = min(right_sceneMinAABB,b_min);
		right_sceneMaxAABB    = max(right_sceneMaxAABB,b_max);
	      }
	      //evictL1(r+2);


	      --r;
	    }
	  if (unlikely(l == r)) {
	    if ( ge_split(r,bestSplitDim,c,s,bestSplit_f))
	      {
		{
		  const mic_f b_min = broadcast4to16f((float*)&r->lower);
		  const mic_f b_max = broadcast4to16f((float*)&r->upper);
		  const mic_f centroid2 = b_min+b_max;
		  right_centroidMinAABB = min(right_centroidMinAABB,centroid2);
		  right_centroidMaxAABB = max(right_centroidMaxAABB,centroid2);
		  right_sceneMinAABB    = min(right_sceneMinAABB,b_min);
		  right_sceneMaxAABB    = max(right_sceneMaxAABB,b_max);
		}	    
		//local_right.extend(*r);
	      }
	    else 
	      l++; 
	    break;
	  }

	  xchg(*l,*r);
	}

      store4f(&local_left.centroid2.lower,left_centroidMinAABB);
      store4f(&local_left.centroid2.upper,left_centroidMaxAABB);
      store4f(&local_left.geometry.lower,left_sceneMinAABB);
      store4f(&local_left.geometry.upper,left_sceneMaxAABB);

      store4f(&local_right.centroid2.lower,right_centroidMinAABB);
      store4f(&local_right.centroid2.upper,right_centroidMaxAABB);
      store4f(&local_right.geometry.lower,right_sceneMinAABB);
      store4f(&local_right.geometry.upper,right_sceneMaxAABB);

      assert( aabb + begin <= l && l <= aabb + end);
      assert( aabb + begin <= r && r <= aabb + end);

      return l - (aabb + begin);
    }

};

#endif
