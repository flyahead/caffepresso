/*
 * This is the program that runs on each Epiphany eCore.
 *
 * Written by: Siddhartha (NTU)
 * Email: siddhart005@e.ntu.edu.sg
 */

#include <stdint.h>
#include "e_lib.h"

#include "types.h"
#include "address.h"
#include "parameters.h"

// global memory management -- stack start address 
asm(".global __stack_start__;");
asm(".set __stack_start__,0x7800;");
asm(".global __heap_start__;");
asm(".set __heap_start__,0x0000;");

FLAG_T *mailboxes;
GLOBAL_CONSTANTS_T *num_maps, *num_patches, *kernel_width, *patch_width, *patch_height, *pool_window_size, *subsample_factor, *big_map_width, *big_map_height, *num_maps_prev, *prev_map_flattened_size, *conv_stride, *pooling_stride, *conv_pad;
ADDR_T *dram_kernel_ptr, *dram_map_ptr, *patch_ptr;
PATCH_T *patch;
KERNEL_T *kernel;
MAP_T *local_map_accum;
SCALE_T *kernel_scale, *window_scale;

//function declarations
void load_kernels(ADDR_T ptr, int n, int k, int coreid);
void reduction(MAP_T *state, int map_size);
void filter2D(KERNEL_T *kernel, MAP_T *src, INTERMEDIATE_T *dest, unsigned kernel_width, unsigned src_width, unsigned src_height, SCALE_T scale, unsigned conv_stride);
void pool(INTERMEDIATE_T *src, unsigned window, unsigned src_width, unsigned src_height, SCALE_T scale, unsigned stride);
void subsample(INTERMEDIATE_T *src, MAP_T *dest, unsigned stride, unsigned src_width, unsigned src_height);
void dump_to_dram(ADDR_T ptr, MAP_T *src, int patch_id, int map_id, unsigned mwidth, unsigned mheight, unsigned big_mwidth, unsigned big_mheight, unsigned coreid, unsigned num_maps);
void load_patch(int patch_num, int map_num, unsigned pw, unsigned ph, ADDR_T ptr, unsigned prev_map_flattened_size);
void pad_patch(PATCH_T *src, PATCH_T *dest, unsigned width, unsigned height, unsigned pad);

int main(void){

	mailboxes = (FLAG_T *)DONE_ADDR; //4 mailbox locations available per core
	*mailboxes = 0xbeefdead;
	mailboxes[1] = 0xdeadbeef;

	unsigned coreid = e_group_config.core_row*4+e_group_config.core_col;

	/*allocate memory based on number of nodes and edges*/
	num_maps = (GLOBAL_CONSTANTS_T *)PARAMETERS_ADDR;
	num_patches = (GLOBAL_CONSTANTS_T *)(num_maps + 1);
	kernel_width = (GLOBAL_CONSTANTS_T *)(num_patches + 1);
	patch_width = (GLOBAL_CONSTANTS_T *)(kernel_width + 1);
	patch_height = (GLOBAL_CONSTANTS_T *)(patch_width + 1);
	pool_window_size = (GLOBAL_CONSTANTS_T *)(patch_height + 1);
	subsample_factor = (GLOBAL_CONSTANTS_T *)(pool_window_size + 1);
	dram_kernel_ptr = (ADDR_T *)(subsample_factor + 1);
	dram_map_ptr = (ADDR_T *)(dram_kernel_ptr + 1);
	patch_ptr = (ADDR_T *)(dram_map_ptr + 1);
	big_map_width = (GLOBAL_CONSTANTS_T *)(patch_ptr + 1);
	big_map_height = (GLOBAL_CONSTANTS_T *)(big_map_width + 1);
	num_maps_prev = (GLOBAL_CONSTANTS_T *)(big_map_height + 1);
	prev_map_flattened_size = (GLOBAL_CONSTANTS_T *)(num_maps_prev + 1);
	pooling_stride = (GLOBAL_CONSTANTS_T *)(prev_map_flattened_size + 1);
	conv_stride = (GLOBAL_CONSTANTS_T *)(pooling_stride + 1);
	conv_pad = (GLOBAL_CONSTANTS_T *)(conv_stride + 1);
	window_scale = (SCALE_T *)(conv_pad + 1); //this is not counted as NUM_PARAMS, since it's float..

	int i,j,k,p;
	
	unsigned accum_map_width = (*patch_width - *kernel_width + 1 + 2*(*conv_pad))/((*subsample_factor)*(*conv_stride));
	unsigned accum_map_height = (*patch_height - *kernel_width + 1 + 2*(*conv_pad))/((*subsample_factor)*(*conv_stride));
	unsigned map_size = accum_map_width*accum_map_height;
	unsigned amw = accum_map_width / (*pooling_stride + *subsample_factor);
	unsigned amh = accum_map_height / (*pooling_stride + *subsample_factor);

	kernel = (KERNEL_T *)(window_scale + 1);
	kernel_scale = (SCALE_T *)(kernel + (*num_maps)*(*kernel_width)*(*kernel_width));
	patch = (PATCH_T *)(kernel_scale + (*num_maps));
	MAP_T *local_map_accum = (MAP_T *)(patch + (*patch_width)*(*patch_height));
	INTERMEDIATE_T *filter2D_out = (INTERMEDIATE_T *)(local_map_accum + map_size);
	unsigned conv_size_width = accum_map_width*(*subsample_factor);
	unsigned conv_size_height = accum_map_height*(*subsample_factor);
	PATCH_T *patch_padded;
	MAP_T *map_patch;
	if (*conv_pad > 0){
		patch_padded = (PATCH_T *)(filter2D_out + conv_size_width*conv_size_height);
		map_patch = (MAP_T *)(patch_padded + (*patch_width+2*(*conv_pad))*(*patch_height+(*conv_pad)));
	} else {
		patch_padded = patch;
		map_patch = (MAP_T *)(filter2D_out + conv_size_width*conv_size_height);
	}

	load_kernels(*dram_kernel_ptr,*num_maps,*kernel_width,coreid);

	for (i=0;i<*num_maps;i++){
		KERNEL_T *ker = (KERNEL_T *)(kernel + (i*(*kernel_width)*(*kernel_width)));
		SCALE_T scale = kernel_scale[i];
		for (p=0;p<*num_patches;p++){
			for (k=0;k<map_size;k++)
				local_map_accum[k] = 0.0f; // initialize accum to zero..
			for (j=0;j<*num_maps_prev;j++){
				load_patch(p,j,*patch_width,*patch_height,*patch_ptr,*prev_map_flattened_size);
				if (*conv_pad > 0){
					pad_patch(patch,patch_padded,*patch_width,*patch_height,*conv_pad);
				}
				filter2D(ker,patch_padded,filter2D_out,*kernel_width,*patch_width,*patch_height,scale,*conv_stride);
				for (k=0;k<map_size;k++)
					local_map_accum[k] += filter2D_out[k]; // accum patch..
			}
			if (*pool_window_size != 0){
				pool(local_map_accum,*pool_window_size,*patch_width,*patch_height,*window_scale,*pooling_stride);
			}
			if (*subsample_factor > 1){
				subsample(local_map_accum,map_patch,*subsample_factor,*patch_width/(*pooling_stride),*patch_height/(*pooling_stride));
			} else {
				map_patch = local_map_accum;
			}
			
			// transfer the map out to DRAM based on coreid.
			dump_to_dram(*dram_map_ptr,map_patch,p,i,amw,amh,*big_map_width,*big_map_height,coreid,*num_maps);
		}
	}

	*mailboxes = 0xdeadbeef;

	return 0;
}

void pad_patch(PATCH_T *src, PATCH_T *dest, unsigned width, unsigned height, unsigned pad){
	int row,col;
	unsigned new_width = width + 2*pad;
	unsigned new_height = height + 2*pad;
	for (row=0;row<new_height;row++){
		for (col=0;col<new_width;col++){
			dest[row*new_width + col] = 0.0f; // init to zero..
		}
	}
	for (row=0;row<height;row++){
		for (col=0;col<width;col++){
			dest[(row+pad)*new_width + col+pad] = src[row*width+col];
		}
	}
}
void dump_to_dram(ADDR_T ptr, MAP_T *src, int patch_id, int map_id, unsigned mwidth, unsigned mheight, unsigned big_mwidth, unsigned big_mheight, unsigned coreid, unsigned num_maps){

	int i;
	unsigned big_map_size = big_mwidth*big_mheight;
	unsigned ppr = big_mwidth/mwidth;
	unsigned col = patch_id%ppr;
	unsigned row = (patch_id-col)/ppr;
	ADDR_T *index = (ADDR_T *)(ptr + (coreid*num_maps + map_id)*big_map_size + row*big_mwidth + col);
	for (i=0;i<mheight;i++){
		ADDR_T *offset = (ADDR_T *)(index + i*big_mwidth);
		MAP_T *src1 = (MAP_T *)(src + i*mwidth);
		e_dma_copy(offset,src1,mwidth*sizeof(MAP_T));
	}

}

void load_kernels(ADDR_T ptr, int n, int k, int coreid){
	ADDR_T *addr = (ADDR_T *)(ptr + n*coreid);
	e_dma_copy(kernel,addr,(n*k*k+n)*sizeof(KERNEL_T));
}

void load_patch(int patch_num, int map_num, unsigned pw, unsigned ph, ADDR_T ptr, unsigned prev_map_flattened_size){
	ADDR_T *addr = (ADDR_T *)(ptr + map_num*prev_map_flattened_size + patch_num*pw*ph);
	e_dma_copy(patch,addr,pw*ph*sizeof(PATCH_T));
}

void filter2D(KERNEL_T *kernel, MAP_T *src, INTERMEDIATE_T *dest, unsigned kernel_width, unsigned src_width, unsigned src_height, SCALE_T scale, unsigned conv_stride){
	unsigned row,col,kernel_row,kernel_col,i;
	int cntr[kernel_width];
	unsigned o_cntr = 0;
	unsigned conv_size_width = src_width - kernel_width + 1;
	unsigned conv_size_height = src_height - kernel_width + 1;
	for (row=0;row<conv_size_height;row+=conv_stride){
		for (i=0;i<kernel_width;i++)
			cntr[i] = (row+i)*src_width;
		for (col=0;col<conv_size_width;col+=conv_stride){
			INTERMEDIATE_T sop = 0.0f;

			for (kernel_row=0;kernel_row<kernel_width;kernel_row++){
				for (kernel_col=0;kernel_col<kernel_width;kernel_col++){
					sop += kernel[kernel_row*kernel_width+kernel_col]*src[cntr[kernel_row]+kernel_col];			
				}
			}
			
			sop = sop*scale;
			*(dest + o_cntr) = sop;

			o_cntr++;
			
			for (i=0;i<kernel_width;i++)
				cntr[i]++;
		}
	}
}

void pool(INTERMEDIATE_T *src, unsigned window, unsigned src_width, unsigned src_height, SCALE_T scale, unsigned stride){
	unsigned row,col,i,j;
	int cntr[window];
	unsigned o_cntr = 0;
	unsigned corr_width = src_width - src_width%stride;
	unsigned corr_height = src_height - src_height%stride;

	for (row=0;row<corr_height;row+=stride){
		for (i=0;i<window;i++)
			cntr[i] = (row+i)*src_width;
		for (col=0;col<corr_width;col+=stride){
			float sum = 0.0f;
			for (i=0;i<window;i++){
				for (j=0;j<window;j++){
					sum += src[cntr[i]+j];
				}
			}
			
			sum = sum*scale; // scale is precomputed..
			
			for (i=0;i<window;i++){
				for (j=0;j<window;j++){
					src[cntr[i]+j] = sum;
				}
			}

			for (i=0;i<window;i++)
				cntr[i] += stride;
		}
	}
}

void subsample(INTERMEDIATE_T *src, MAP_T *dest, unsigned stride, unsigned src_width, unsigned src_height){
	unsigned row,col,i,j;
	int cntr[stride];
	unsigned o_cntr = 0;

	unsigned corr_width = src_width - src_width%stride;
	unsigned corr_height = src_height - src_height%stride;

	for (row=0;row<corr_height;row+=stride){
		for (i=0;i<stride;i++)
			cntr[i] = (row+i)*src_width;
		for (col=0;col<corr_width;col+=stride){

			dest[o_cntr] = src[row*src_width+col];
			o_cntr++;

			for (i=0;i<stride;i++)
				cntr[i] += stride;
		}
	}
}
