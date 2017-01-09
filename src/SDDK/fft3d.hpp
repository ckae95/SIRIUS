// Copyright (c) 2013-2016 Anton Kozhevnikov, Thomas Schulthess
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are permitted provided that 
// the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the 
//    following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions 
//    and the following disclaimer in the documentation and/or other materials provided with the distribution.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED 
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A 
// PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR 
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, 
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER 
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR 
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

/** \file fft3d.hpp
 *   
 *  \brief Contains declaration and partial implementation of FFT3D class.
 */

#ifndef __FFT3D_HPP__
#define __FFT3D_HPP__

#include <fftw3.h>
#include "vector3d.hpp"
#include "fft3d_grid.hpp"
#include "gvec.hpp"

namespace sddk {

#ifdef __GPU
extern "C" void unpack_z_cols_gpu(cuDoubleComplex* z_cols_packed__,
                                  cuDoubleComplex* fft_buf__,
                                  int size_x__,
                                  int size_y__,
                                  int size_z__,
                                  int num_z_cols__,
                                  int const* z_columns_pos__,
                                  bool use_reduction,
                                  int stream_id__);

extern "C" void unpack_z_cols_2_gpu(cuDoubleComplex* z_cols_packed1__,
                                    cuDoubleComplex* z_cols_packed2__,
                                    cuDoubleComplex* fft_buf__,
                                    int size_x__,
                                    int size_y__,
                                    int size_z__,
                                    int num_z_cols__,
                                    int const* z_columns_pos__,
                                    int stream_id__);

extern "C" void pack_z_cols_gpu(cuDoubleComplex* z_cols_packed__,
                                cuDoubleComplex* fft_buf__,
                                int size_x__,
                                int size_y__,
                                int size_z__,
                                int num_z_cols__,
                                int const* z_columns_pos__,
                                int stream_id__);

extern "C" void pack_z_cols_2_gpu(cuDoubleComplex* z_cols_packed1__,
                                  cuDoubleComplex* z_cols_packed2__,
                                  cuDoubleComplex* fft_buf__,
                                  int size_x__,
                                  int size_y__,
                                  int size_z__,
                                  int num_z_cols__,
                                  int const* z_columns_pos__,
                                  int stream_id__);

extern "C" void cufft_batch_load_gpu(int fft_size,
                                     int num_pw_components, 
                                     int num_fft,
                                     int const* map, 
                                     cuDoubleComplex* data, 
                                     cuDoubleComplex* fft_buffer);

extern "C" void cufft_batch_unload_gpu(int fft_size,
                                       int num_pw_components,
                                       int num_fft,
                                       int const* map, 
                                       cuDoubleComplex const* fft_buffer, 
                                       cuDoubleComplex* data,
                                       double alpha,
                                       double beta);

#endif

//#define __CUFFT3D

/// Implementation of FFT3D.
/** FFT convention:
 *  \f[
 *      f({\bf r}) = \sum_{{\bf G}} e^{i{\bf G}{\bf r}} f({\bf G})
 *  \f]
 *  is a \em backward transformation from a set of pw coefficients to a function.  
 *
 *  \f[
 *      f({\bf G}) = \frac{1}{\Omega} \int e^{-i{\bf G}{\bf r}} f({\bf r}) d {\bf r} = 
 *          \frac{1}{N} \sum_{{\bf r}_j} e^{-i{\bf G}{\bf r}_j} f({\bf r}_j)
 *  \f]
 *  is a \em forward transformation from a function to a set of coefficients. 
 */
class FFT3D
{
    protected:
        
        /// Communicator for the parallel FFT.
        Communicator const& comm_;

        /// Main processing unit of this FFT.
        device_t pu_;
        
        /// Split z-direction.
        splindex<block> spl_z_;
        
        /// Definition of the FFT grid.
        FFT3D_grid grid_;

        /// Local size of z-dimension of FFT buffer.
        int local_size_z_;

        /// Offset in the global z-dimension.
        int offset_z_;

        /// Main input/output buffer.
        mdarray<double_complex, 1> fft_buffer_;
        
        /// Auxiliary array to store z-sticks for all-to-all or GPU.
        mdarray<double_complex, 1> fft_buffer_aux1_;
        
        /// Auxiliary array in case of simultaneous transformation of two wave-functions.
        mdarray<double_complex, 1> fft_buffer_aux2_;
        
        /// Internal buffer for independent z-transforms.
        std::vector<double_complex*> fftw_buffer_z_;

        /// Internal buffer for independent {xy}-transforms.
        std::vector<double_complex*> fftw_buffer_xy_;

        std::vector<fftw_plan> plan_backward_z_;

        std::vector<fftw_plan> plan_backward_xy_;
        
        std::vector<fftw_plan> plan_forward_z_;

        std::vector<fftw_plan> plan_forward_xy_;

        #ifdef __GPU
        bool gpu_only_impl_{false};
        cufftHandle cufft_plan_xy_;
        cufftHandle cufft_plan_z_;
        cufftHandle cufft_plan_3d_;
        mdarray<char, 1> cufft_work_buf_;
        int cufft_nbatch_xy_{0};
        int cufft_nbatch_z_{0};

        /// Mapping of G-vectors of z-columns to the FFT buffer for batched 1D transform.
        mdarray<int, 1> z_col_map_;
        #endif

        /// Position of z-columns inside 2D FFT buffer. 
        mdarray<int, 2> z_col_pos_;

        bool prepared_{false};

        #ifdef __GPU
        /// Whole FFT transformation on a GPU.
        template <int direction, bool use_reduction>
        void transform_3d_serial_gpu(Gvec_partition const& gvec__, double_complex* data__)
        {
            switch (direction) {
                case 1: {
                    #ifdef __CUFFT3D
                    cufft_batch_load_gpu(grid_.size(), gvec__.num_gvec(), 1, 
                                         z_col_map_.at<GPU>(), data__, fft_buffer_.at<GPU>());
                    cufft_backward_transform(cufft_plan_3d_, fft_buffer_.at<GPU>());
                    #else
                    /* load all columns into FFT buffer */
                    cufft_batch_load_gpu(gvec__.num_zcol() * grid_.size(2), gvec__.num_gvec(), 1, 
                                         z_col_map_.at<GPU>(), data__, fft_buffer_aux1_.at<GPU>());
                    if (use_reduction) {
                        STOP();
                        /* add stuff */
                    }
                    /* transform all columns */
                    cufft_backward_transform(cufft_plan_z_, fft_buffer_aux1_.at<GPU>());
                    /* unpack z-columns into proper position of the FFT buffer */
                    unpack_z_cols_gpu(fft_buffer_aux1_.at<GPU>(), fft_buffer_.at<GPU>(), grid_.size(0), grid_.size(1), 
                                      cufft_nbatch_xy_, gvec__.num_zcol(), z_col_pos_.at<GPU>(), use_reduction, 0);
                    /* execute FFT */
                    cufft_backward_transform(cufft_plan_xy_, fft_buffer_.at<GPU>());
                    #endif
                    break;
                }
                case -1: {
                    #ifdef __CUFFT3D
                    cufft_forward_transform(cufft_plan_3d_, fft_buffer_.at<GPU>());
                    cufft_batch_unload_gpu(grid_.size(), gvec__.num_gvec(), 1, 
                                           z_col_map_.at<GPU>(), fft_buffer_.at<GPU>(), data__, 0.0, 1.0 / size());
                    #else
                    /* executes FFT */
                    cufft_forward_transform(cufft_plan_xy_, fft_buffer_.at<GPU>());
                    /* pack z-columns */
                    pack_z_cols_gpu(fft_buffer_aux1_.at<GPU>(), fft_buffer_.at<GPU>(), grid_.size(0), grid_.size(1), 
                                    cufft_nbatch_xy_, gvec__.num_zcol(), z_col_pos_.at<GPU>(), 0);

                    /* transform all columns */
                    cufft_forward_transform(cufft_plan_z_, fft_buffer_aux1_.at<GPU>());
                    acc::zero(data__, gvec__.num_gvec());
                    /* get all columns from FFT buffer */
                    cufft_batch_unload_gpu(gvec__.num_zcol() * grid_.size(2), gvec__.num_gvec(), 1, 
                                           z_col_map_.at<GPU>(), fft_buffer_aux1_.at<GPU>(), data__, 0.0, 1.0 / size());
                    #endif
                    break;
                }
                default: {
                    TERMINATE("wrong FFT direction");
                }
            }
            /* stream#0 is doing a job */
            acc::sync_stream(0);
        }
        #endif
        
        /// Transform z-columns.
        template <int direction, bool use_reduction>
        void transform_z_serial(Gvec_partition const& gvec__,
                                double_complex* data__,
                                mdarray<double_complex, 1>& fft_buffer_aux__)
        {
            PROFILE("sddk::FFT3D::transform_z_serial");

            double norm = 1.0 / size();
            #pragma omp parallel
            {
                int tid = omp_get_thread_num();
                #pragma omp for schedule(dynamic, 1)
                for (int i = 0; i < gvec__.num_zcol(); i++) {
                    int data_offset = gvec__.zcol_offs(i);

                    switch (direction) {
                        case 1: {
                            /* zero input FFT buffer */
                            std::fill(fftw_buffer_z_[tid], fftw_buffer_z_[tid] + grid_.size(2), 0);
                            /* load column into local FFT buffer */
                            for (size_t j = 0; j < gvec__.zcol(i).z.size(); j++) {
                                /* coordinate inside FFT grid */
                                int z = grid().coord_by_gvec(gvec__.zcol(i).z[j], 2);

                                fftw_buffer_z_[tid][z] = data__[data_offset + j];
                            }
                            /* column with {x,y} = {0,0} has only non-negative z components */
                            if (use_reduction && !i) {
                                /* load remaining part of {0,0,z} column */
                                for (size_t j = 0; j < gvec__.zcol(0).z.size(); j++) {
                                    int z = grid().coord_by_gvec(-gvec__.zcol(i).z[j], 2);
                                    fftw_buffer_z_[tid][z] = std::conj(data__[data_offset + j]);
                                }
                            }

                            /* execute 1D transform of a z-column */
                            fftw_execute(plan_backward_z_[tid]);

                            /* load full column into auxiliary buffer */
                            std::copy(fftw_buffer_z_[tid], fftw_buffer_z_[tid] + grid_.size(2), &fft_buffer_aux__[i * grid_.size(2)]);

                            break;
                        }
                        case -1: {
                            /* load full column from auxiliary buffer */
                            std::copy(&fft_buffer_aux__[i * grid_.size(2)], &fft_buffer_aux__[i * grid_.size(2)] + grid_.size(2), fftw_buffer_z_[tid]);

                            /* execute 1D transform of a z-column */
                            fftw_execute(plan_forward_z_[tid]);

                            /* store PW coefficients */
                            for (size_t j = 0; j < gvec__.zcol(i).z.size(); j++) {
                                int z = grid().coord_by_gvec(gvec__.zcol(i).z[j], 2);
                                data__[data_offset + j] = fftw_buffer_z_[tid][z] * norm;
                            }
                            break;
                        }
                        default: {
                            TERMINATE("wrong direction");
                        }
                    }
                }
            }
        }

        /// Parallel transform of z-columns.
        template <int direction, bool use_reduction>
        void transform_z_parallel(Gvec_partition const& gvec__,
                                  double_complex* data__,
                                  mdarray<double_complex, 1>& fft_buffer_aux__)
        {
            PROFILE("sddk::FFT3D::transform_z_parallel");

            int rank = comm_.rank();
            int num_zcol_local = gvec__.zcol_distr_fft().counts[rank];
            double norm = 1.0 / size();

            if (direction == -1) {
                sddk::timer t("sddk::FFT3D::transform_z_parallel|comm");

                block_data_descriptor send(comm_.size());
                block_data_descriptor recv(comm_.size());
                for (int r = 0; r < comm_.size(); r++) {
                    send.counts[r] = spl_z_.local_size(rank) * gvec__.zcol_distr_fft().counts[r];
                    recv.counts[r] = spl_z_.local_size(r)    * gvec__.zcol_distr_fft().counts[rank];
                }
                send.calc_offsets();
                recv.calc_offsets();

                std::copy(&fft_buffer_aux__[0], &fft_buffer_aux__[0] + gvec__.num_zcol() * local_size_z_,
                          &fft_buffer_[0]);

                comm_.alltoall(&fft_buffer_[0], &send.counts[0], &send.offsets[0], &fft_buffer_aux__[0], &recv.counts[0], &recv.offsets[0]);
            }

            #pragma omp parallel
            {
                int tid = omp_get_thread_num();
                #pragma omp for schedule(dynamic, 1)
                for (int i = 0; i < num_zcol_local; i++) {
                    /* global index of column */
                    int icol = gvec__.zcol_distr_fft().offsets[rank] + i;
                    int data_offset = gvec__.zcol_offs(icol);

                    switch (direction) {
                        case 1: {
                            /* clear z buffer */
                            std::fill(fftw_buffer_z_[tid], fftw_buffer_z_[tid] + grid_.size(2), 0);
                            /* load z column  of PW coefficients into buffer */
                            for (size_t j = 0; j < gvec__.zcol(icol).z.size(); j++) {
                                int z = grid().coord_by_gvec(gvec__.zcol(icol).z[j], 2);
                                fftw_buffer_z_[tid][z] = data__[data_offset + j];
                            }

                            /* column with {x,y} = {0,0} has only non-negative z components */
                            if (use_reduction && !icol) {
                                /* load remaining part of {0,0,z} column */
                                for (size_t j = 0; j < gvec__.zcol(icol).z.size(); j++) {
                                    int z = grid().coord_by_gvec(-gvec__.zcol(icol).z[j], 2);
                                    fftw_buffer_z_[tid][z] = std::conj(data__[data_offset + j]);
                                }
                            }

                            /* perform local FFT transform of a column */
                            fftw_execute(plan_backward_z_[tid]);

                            /* redistribute z-column for a forthcoming all-to-all */ 
                            for (int r = 0; r < comm_.size(); r++) {
                                int lsz = spl_z_.local_size(r);
                                int offs = spl_z_.global_offset(r);

                                std::copy(&fftw_buffer_z_[tid][offs], &fftw_buffer_z_[tid][offs + lsz], 
                                          &fft_buffer_aux__[offs * num_zcol_local + i * lsz]);
                            }

                            break;

                        }
                        case -1: {
                            /* collect full z-column */ 
                            for (int r = 0; r < comm_.size(); r++) {
                                int lsz = spl_z_.local_size(r);
                                int offs = spl_z_.global_offset(r);
                                std::copy(&fft_buffer_aux__[offs * num_zcol_local + i * lsz],
                                          &fft_buffer_aux__[offs * num_zcol_local + i * lsz + lsz],
                                          &fftw_buffer_z_[tid][offs]);
                            }

                            /* perform local FFT transform of a column */
                            fftw_execute(plan_forward_z_[tid]);

                            /* save z column of PW coefficients*/
                            for (size_t j = 0; j < gvec__.zcol(icol).z.size(); j++) {
                                int z = grid().coord_by_gvec(gvec__.zcol(icol).z[j], 2);
                                data__[data_offset + j] = fftw_buffer_z_[tid][z] * norm;
                            }
                            break;

                        }
                        default: {
                            TERMINATE("wrong direction");
                        }
                    }
                }
            }

            /* scatter z-columns between slabs of FFT buffer */
            if (direction == 1) {
                sddk::timer t("sddk::FFT3D::transform_z_parallel|comm");

                block_data_descriptor send(comm_.size());
                block_data_descriptor recv(comm_.size());
                for (int r = 0; r < comm_.size(); r++) {
                    send.counts[r] = spl_z_.local_size(r)    * gvec__.zcol_distr_fft().counts[rank];
                    recv.counts[r] = spl_z_.local_size(rank) * gvec__.zcol_distr_fft().counts[r];
                }
                send.calc_offsets();
                recv.calc_offsets();

                /* scatter z-columns */
                comm_.alltoall(&fft_buffer_aux__[0], &send.counts[0], &send.offsets[0], &fft_buffer_[0], &recv.counts[0], &recv.offsets[0]);

                /* copy local fractions of z-columns into auxiliary buffer */
                std::copy(&fft_buffer_[0], &fft_buffer_[0] + gvec__.num_zcol() * local_size_z_,
                          &fft_buffer_aux__[0]);
            }
        } 
        
        /// Apply 2D FFT transformation to z-columns of one complex function.
        template <int direction, bool use_reduction>
        void transform_xy(Gvec_partition const& gvec__,
                          mdarray<double_complex, 1>& fft_buffer_aux__)
        {
            PROFILE("sddk::FFT3D::transform_xy");

            int size_xy = grid_.size(0) * grid_.size(1);
            int first_z{0};

            #ifdef __GPU
            if (pu_ == GPU) {
                /* stream #0 will be doing cuFFT */
                switch (direction) {
                    case 1: {
                        /* srteam #0 copies packed columns to GPU */
                        acc::copyin(fft_buffer_aux__.at<GPU>(), cufft_nbatch_xy_, fft_buffer_aux__.at<CPU>(), local_size_z_,
                                    cufft_nbatch_xy_, gvec__.num_zcol(), 0);
                        /* srteam #0 unpacks z-columns into proper position of FFT buffer */
                        unpack_z_cols_gpu(fft_buffer_aux__.at<GPU>(), fft_buffer_.at<GPU>(), grid_.size(0), grid_.size(1), 
                                          cufft_nbatch_xy_, gvec__.num_zcol(), z_col_pos_.at<GPU>(), use_reduction, 0);
                        /* stream #0 executes FFT */
                        cufft_backward_transform(cufft_plan_xy_, fft_buffer_.at<GPU>());
                        break;
                    }
                    case -1: {
                        if (!gpu_only_impl_) {
                            /* stream #1 copies part of FFT buffer to CPU */
                            acc::copyout(fft_buffer_.at<CPU>(cufft_nbatch_xy_ * size_xy), fft_buffer_.at<GPU>(cufft_nbatch_xy_ * size_xy),
                                         size_xy * (local_size_z_ - cufft_nbatch_xy_), 1);
                        }
                        /* stream #0 executes FFT */
                        cufft_forward_transform(cufft_plan_xy_, fft_buffer_.at<GPU>());
                        /* stream #0 packs z-columns */
                        pack_z_cols_gpu(fft_buffer_aux__.at<GPU>(), fft_buffer_.at<GPU>(), grid_.size(0), grid_.size(1), 
                                        cufft_nbatch_xy_, gvec__.num_zcol(), z_col_pos_.at<GPU>(), 0);
                        /* srteam #0 copies packed columns to CPU */
                        acc::copyout(fft_buffer_aux__.at<CPU>(), local_size_z_, fft_buffer_aux__.at<GPU>(), cufft_nbatch_xy_, 
                                     cufft_nbatch_xy_, gvec__.num_zcol(), 0);
                        /* stream #1 waits to complete memory copy */
                        acc::sync_stream(1);
                        break;
                    }
                }
                first_z = cufft_nbatch_xy_;
            }
            #endif
            
            #pragma omp parallel
            {
                int tid = omp_get_thread_num();
                #pragma omp for
                for (int iz = first_z; iz < local_size_z_; iz++) {
                    switch (direction) {
                        case 1: {
                            /* clear xy-buffer */
                            std::fill(fftw_buffer_xy_[tid], fftw_buffer_xy_[tid] + size_xy, 0);
                            /* load z-columns into proper location */
                            for (int i = 0; i < gvec__.num_zcol(); i++) {
                                fftw_buffer_xy_[tid][z_col_pos_(i, 0)] = fft_buffer_aux__[iz + i * local_size_z_];

                                if (use_reduction && i) {
                                    fftw_buffer_xy_[tid][z_col_pos_(i, 1)] = std::conj(fftw_buffer_xy_[tid][z_col_pos_(i, 0)]);
                                }
                            }
                            
                            /* execute local FFT transform */
                            fftw_execute(plan_backward_xy_[tid]);

                            /* copy xy plane to the main FFT buffer */
                            std::copy(fftw_buffer_xy_[tid], fftw_buffer_xy_[tid] + size_xy, &fft_buffer_[iz * size_xy]);
                            
                            break;
                        }
                        case -1: {
                            /* copy xy plane from the main FFT buffer */
                            std::copy(&fft_buffer_[iz * size_xy], &fft_buffer_[iz * size_xy] + size_xy, fftw_buffer_xy_[tid]);

                            /* execute local FFT transform */
                            fftw_execute(plan_forward_xy_[tid]);

                            /* get z-columns */
                            for (int i = 0; i < gvec__.num_zcol(); i++) {
                                fft_buffer_aux__[iz  + i * local_size_z_] = fftw_buffer_xy_[tid][z_col_pos_(i, 0)];
                            }

                            break;
                        }
                        default: {
                            TERMINATE("wrong direction");
                        }
                    }
                }
            }
                
            #ifdef __GPU
            if (pu_ == GPU) {
                if (direction == 1 && !gpu_only_impl_) {
                    /* stream #1 copies data to GPU */
                    acc::copyin(fft_buffer_.at<GPU>(cufft_nbatch_xy_ * size_xy), fft_buffer_.at<CPU>(cufft_nbatch_xy_ * size_xy),
                                size_xy * (local_size_z_ - cufft_nbatch_xy_), 1);
                }
                /* wait for stram #0 */
                acc::sync_stream(0);
                /* wait for stram #1 */
                acc::sync_stream(1);
            }
            #endif
        }

        /// Apply 2D FFT transformation to z-columns of two real functions.
        template <int direction>
        void transform_xy(Gvec_partition const& gvec__,
                          mdarray<double_complex, 1>& fft_buffer_aux1__, 
                          mdarray<double_complex, 1>& fft_buffer_aux2__)
        {
            PROFILE("sddk::FFT3D::transform_xy");

            if (!gvec__.reduced()) {
                TERMINATE("reduced set of G-vectors is required");
            }

            int size_xy = grid_.size(0) * grid_.size(1);
            int first_z = 0;

            #ifdef __GPU
            if (pu_ == GPU) {
                /* stream #0 will be doing cuFFT */
                switch (direction) {
                    case 1: {
                        /* srteam #0 copies packed columns to GPU */
                        acc::copyin(fft_buffer_aux1__.at<GPU>(), cufft_nbatch_xy_,
                                    fft_buffer_aux1__.at<CPU>(), local_size_z_,
                                    cufft_nbatch_xy_, gvec__.num_zcol(), 0);
                        acc::copyin(fft_buffer_aux2__.at<GPU>(), cufft_nbatch_xy_,
                                    fft_buffer_aux2__.at<CPU>(), local_size_z_,
                                    cufft_nbatch_xy_, gvec__.num_zcol(), 0);
                        /* srteam #0 unpacks z-columns into proper position of FFT buffer */
                        unpack_z_cols_2_gpu(fft_buffer_aux1__.at<GPU>(), fft_buffer_aux2__.at<GPU>(), fft_buffer_.at<GPU>(),
                                            grid_.size(0), grid_.size(1), cufft_nbatch_xy_,
                                            gvec__.num_zcol(), z_col_pos_.at<GPU>(), 0);
                        /* stream #0 executes FFT */
                        cufft_backward_transform(cufft_plan_xy_, fft_buffer_.at<GPU>());
                        break;
                    }
                    case -1: {
                        /* if we do some part on CPU */
                        if (!gpu_only_impl_) {
                            /* stream #1 copies part of FFT buffer to CPU */
                            acc::copyout(fft_buffer_.at<CPU>(cufft_nbatch_xy_ * size_xy), fft_buffer_.at<GPU>(cufft_nbatch_xy_ * size_xy),
                                         size_xy * (local_size_z_ - cufft_nbatch_xy_), 1);
                        }
                        /* stream #0 executes FFT */
                        cufft_forward_transform(cufft_plan_xy_, fft_buffer_.at<GPU>());
                        /* stream #0 packs z-columns */
                        pack_z_cols_2_gpu(fft_buffer_aux1__.at<GPU>(), fft_buffer_aux2__.at<GPU>(), fft_buffer_.at<GPU>(),
                                          grid_.size(0), grid_.size(1), cufft_nbatch_xy_,
                                          gvec__.num_zcol(), z_col_pos_.at<GPU>(), 0);
                        /* srteam #0 copies packed columns to CPU */
                        acc::copyout(fft_buffer_aux1__.at<CPU>(), local_size_z_,
                                     fft_buffer_aux1__.at<GPU>(), cufft_nbatch_xy_, 
                                     cufft_nbatch_xy_, gvec__.num_zcol(), 0);
                        acc::copyout(fft_buffer_aux2__.at<CPU>(), local_size_z_,
                                     fft_buffer_aux2__.at<GPU>(), cufft_nbatch_xy_, 
                                     cufft_nbatch_xy_, gvec__.num_zcol(), 0);
                        /* stream #1 waits to complete memory copy */
                        acc::sync_stream(1);
                        break;
                    }
                }
                first_z = cufft_nbatch_xy_;
            }
            #endif

            #pragma omp parallel
            {
                int tid = omp_get_thread_num();
                #pragma omp for
                for (int iz = first_z; iz < local_size_z_; iz++) {
                    switch (direction) {
                        case 1: {
                            /* clear xy-buffer */
                            std::fill(fftw_buffer_xy_[tid], fftw_buffer_xy_[tid] + size_xy, 0);

                            /* load first z-column into proper location */
                            fftw_buffer_xy_[tid][z_col_pos_(0, 0)] = fft_buffer_aux1__[iz] + 
                                double_complex(0, 1) * fft_buffer_aux2__[iz];

                            /* load remaining z-columns into proper location */
                            for (int i = 1; i < gvec__.num_zcol(); i++) {
                                /* {x, y} part */
                                fftw_buffer_xy_[tid][z_col_pos_(i, 0)] = fft_buffer_aux1__[iz + i * local_size_z_] + 
                                    double_complex(0, 1) * fft_buffer_aux2__[iz + i * local_size_z_];

                                /* {-x, -y} part */
                                fftw_buffer_xy_[tid][z_col_pos_(i, 1)] = std::conj(fft_buffer_aux1__[iz + i * local_size_z_]) +
                                    double_complex(0, 1) * std::conj(fft_buffer_aux2__[iz + i * local_size_z_]);
                            }
                            
                            /* execute local FFT transform */
                            fftw_execute(plan_backward_xy_[tid]);

                            /* copy xy plane to the main FFT buffer */
                            std::copy(fftw_buffer_xy_[tid], fftw_buffer_xy_[tid] + size_xy, &fft_buffer_[iz * size_xy]);
                            
                            break;
                        }
                        case -1: {
                            /* copy xy plane from the main FFT buffer */
                            std::copy(&fft_buffer_[iz * size_xy], &fft_buffer_[iz * size_xy] + size_xy, fftw_buffer_xy_[tid]);

                            /* execute local FFT transform */
                            fftw_execute(plan_forward_xy_[tid]);

                            /* get z-columns */
                            for (int i = 0; i < gvec__.num_zcol(); i++) {
                                fft_buffer_aux1__[iz  + i * local_size_z_] = 0.5 * 
                                    (fftw_buffer_xy_[tid][z_col_pos_(i, 0)] + std::conj(fftw_buffer_xy_[tid][z_col_pos_(i, 1)]));

                                fft_buffer_aux2__[iz  + i * local_size_z_] = double_complex(0, -0.5) * 
                                    (fftw_buffer_xy_[tid][z_col_pos_(i, 0)] - std::conj(fftw_buffer_xy_[tid][z_col_pos_(i, 1)]));
                            }

                            break;
                        }
                        default: {
                            TERMINATE("wrong direction");
                        }
                    }
                }
            }
                
            #ifdef __GPU
            if (pu_ == GPU) {
                if (direction == 1 && !gpu_only_impl_) {
                    /* stream #1 copies data to GPU */
                    acc::copyin(fft_buffer_.at<GPU>(cufft_nbatch_xy_ * size_xy), fft_buffer_.at<CPU>(cufft_nbatch_xy_ * size_xy),
                                size_xy * (local_size_z_ - cufft_nbatch_xy_));
                }
                /* wait for stram #0 */
                acc::sync_stream(0);
            }
            #endif
        }

    public:
        
        /// Constructor.
        FFT3D(FFT3D_grid grid__,
              Communicator const& comm__,
              device_t pu__,
              double gpu_workload = 0.8)
            : comm_(comm__),
              pu_(pu__),
              grid_(grid__)
        {
            PROFILE("sddk::FFT3D::FFT3D");

            /* split z-direction */
            spl_z_ = splindex<block>(grid_.size(2), comm_.size(), comm_.rank());
            local_size_z_ = spl_z_.local_size();
            offset_z_ = spl_z_.global_offset();

            /* allocate main buffer */
            fft_buffer_ = mdarray<double_complex, 1>(local_size(), memory_t::host_pinned, "FFT3D.fft_buffer_");
            
            /* allocate 1d and 2d buffers */
            for (int i = 0; i < omp_get_max_threads(); i++) {
                fftw_buffer_z_.push_back((double_complex*)fftw_malloc(grid_.size(2) * sizeof(double_complex)));
                fftw_buffer_xy_.push_back((double_complex*)fftw_malloc(grid_.size(0) * grid_.size(1) * sizeof(double_complex)));
            }

            plan_forward_z_   = std::vector<fftw_plan>(omp_get_max_threads());
            plan_forward_xy_  = std::vector<fftw_plan>(omp_get_max_threads());
            plan_backward_z_  = std::vector<fftw_plan>(omp_get_max_threads());
            plan_backward_xy_ = std::vector<fftw_plan>(omp_get_max_threads());

            for (int i = 0; i < omp_get_max_threads(); i++) {
                plan_forward_z_[i] = fftw_plan_dft_1d(grid_.size(2), (fftw_complex*)fftw_buffer_z_[i], 
                                                      (fftw_complex*)fftw_buffer_z_[i], FFTW_FORWARD, FFTW_ESTIMATE);

                plan_backward_z_[i] = fftw_plan_dft_1d(grid_.size(2), (fftw_complex*)fftw_buffer_z_[i], 
                                                       (fftw_complex*)fftw_buffer_z_[i], FFTW_BACKWARD, FFTW_ESTIMATE);
                
                plan_forward_xy_[i] = fftw_plan_dft_2d(grid_.size(1), grid_.size(0), (fftw_complex*)fftw_buffer_xy_[i], 
                                                       (fftw_complex*)fftw_buffer_xy_[i], FFTW_FORWARD, FFTW_ESTIMATE);

                plan_backward_xy_[i] = fftw_plan_dft_2d(grid_.size(1), grid_.size(0), (fftw_complex*)fftw_buffer_xy_[i], 
                                                        (fftw_complex*)fftw_buffer_xy_[i], FFTW_BACKWARD, FFTW_ESTIMATE);
            }
            
            #ifdef __GPU
            if (pu_ == GPU) {
                int auto_alloc{0};
                /* GPU will take care of this number of xy-planes */
                cufft_nbatch_xy_ = static_cast<int>(gpu_workload * local_size_z_ + 1e-12);

                int dim_xy[] = {grid_.size(1), grid_.size(0)};

                cufft_create_plan_handle(&cufft_plan_xy_);
                cufft_create_batch_plan(cufft_plan_xy_, 2, dim_xy, dim_xy, 1, grid_.size(0) * grid_.size(1), cufft_nbatch_xy_, auto_alloc);
                /* stream #0 will execute FFTs */
                cufft_set_stream(cufft_plan_xy_, 0);

                #ifdef __CUFFT3D
                if (comm_.size() != 1) {
                    TERMINATE("CUFFT3D can't be parallel");
                }
                gpu_only_impl_ = true;
                cufft_create_plan_handle(&cufft_plan_3d_);
                int dims[] = {grid_.size(2), grid_.size(1), grid_.size(0)};
                cufft_create_batch_plan(cufft_plan_3d_, 3, dims, dims, 1, grid_.size(2) * grid_.size(1) * grid_.size(0), 1, 0);
                cufft_set_stream(cufft_plan_3d_, 0);
                #else
                if (comm_.size() == 1 && cufft_nbatch_xy_ == grid_.size(2)) {
                    gpu_only_impl_ = true;
                    cufft_create_plan_handle(&cufft_plan_z_);
                    cufft_set_stream(cufft_plan_z_, 0);
                }
                #endif
            }
            #endif
        }
        
        /// Destructor.
        ~FFT3D()
        {
            for (int i = 0; i < omp_get_max_threads(); i++) {
                fftw_free(fftw_buffer_z_[i]);
                fftw_free(fftw_buffer_xy_[i]);

                fftw_destroy_plan(plan_forward_z_[i]);
                fftw_destroy_plan(plan_forward_xy_[i]);
                fftw_destroy_plan(plan_backward_z_[i]);
                fftw_destroy_plan(plan_backward_xy_[i]);
            }
            #ifdef __GPU
            if (pu_ == GPU) {
                cufft_destroy_plan_handle(cufft_plan_xy_);
                #ifdef __CUFFT3D
                cufft_destroy_plan_handle(cufft_plan_3d_);
                #else
                if (gpu_only_impl_) {
                    cufft_destroy_plan_handle(cufft_plan_z_);
                }
                #endif
            }
            #endif
        }

        //template<typename T>
        //inline void input(int n__, int const* map__, T const* data__)
        //{
        //    memset(fftw_buffer_, 0, local_size() * sizeof(double_complex));
        //    for (int i = 0; i < n__; i++) fftw_buffer_[map__[i]] = data__[i];
        //}

        template <typename T>
        inline void input(T* data__)
        {
            for (int i = 0; i < local_size(); i++) {
                fft_buffer_[i] = data__[i];
            }
            #ifdef __GPU
            if (pu_ == GPU) {
                fft_buffer_.copy_to_device();
            }
            #endif
        }
        
        inline void output(double* data__)
        {
            #ifdef __GPU
            if (pu_ == GPU) {
                fft_buffer_.copy_to_host();
            }
            #endif
            for (int i = 0; i < local_size(); i++) {
                data__[i] = fft_buffer_[i].real();
            }
        }
        
        inline void output(double_complex* data__)
        {
            switch (pu_) {
                case CPU: {
                    std::memcpy(data__, fft_buffer_.at<CPU>(), local_size() * sizeof(double_complex));
                    break;
                }
                case GPU: {
                    #ifdef __GPU
                    acc::copyout(data__, fft_buffer_.at<GPU>(), local_size());
                    #endif
                    break;
                }
            }
        }
        
        FFT3D_grid const& grid() const
        {
            return grid_;
        }
        
        /// Total size of the FFT grid.
        inline int size() const
        {
            return grid_.size();
        }

        inline int local_size() const
        {
            return grid_.size(0) * grid_.size(1) * local_size_z_;
        }

        inline int local_size_z() const
        {
            return local_size_z_;
        }

        inline int offset_z() const
        {
            return offset_z_;
        }

        /// Direct access to the fft buffer
        inline double_complex& buffer(int idx__)
        {
            return fft_buffer_[idx__];
        }
        
        template <device_t pu>
        inline double_complex* buffer()
        {
            return fft_buffer_.at<pu>();
        }

        inline mdarray<double_complex, 1>& buffer()
        {
            return fft_buffer_;
        }
        
        Communicator const& comm() const
        {
            return comm_;
        }

        inline bool parallel() const
        {
            return (comm_.size() != 1);
        }
        
        /// True if GPU device is used.
        inline bool hybrid() const
        {
            return (pu_ == GPU);
        }

        /// True if fully executed on a GPU device.
        inline bool gpu_only() const
        {
            #ifdef __GPU
            return gpu_only_impl_;
            #else
            return false;
            #endif
        }

        /// Prepare FFT driver to transfrom functions with gvec_fft_distr.
        void prepare(Gvec_partition const& gvec__)
        {
            PROFILE("sddk::FFT3D::prepare");

            int nc = gvec__.reduced() ? 2 : 1;

            z_col_pos_ = mdarray<int, 2>(gvec__.num_zcol(), nc, memory_t::host, "FFT3D.z_col_pos_");
            #pragma omp parallel for
            for (int i = 0; i < gvec__.num_zcol(); i++) {
                int x = grid().coord_by_gvec(gvec__.zcol(i).x, 0);
                int y = grid().coord_by_gvec(gvec__.zcol(i).y, 1);
                z_col_pos_(i, 0) = x + y * grid_.size(0);
                if (gvec__.reduced()) {
                    x = grid().coord_by_gvec(-gvec__.zcol(i).x, 0);
                    y = grid().coord_by_gvec(-gvec__.zcol(i).y, 1);
                    z_col_pos_(i, 1) = x + y * grid_.size(0);
                }
            }

            #ifdef __GPU
            if (pu_ == GPU) {
                /* for the full GPU transform */
                size_t work_size;
                if (gpu_only_impl_) {
                    z_col_map_ = mdarray<int, 1>(gvec__.num_gvec(), memory_t::host | memory_t::device, "FFT3D.z_col_map_");
                    #pragma omp parallel for
                    for (int i = 0; i < gvec__.num_zcol(); i++) {
                        for (size_t j = 0; j < gvec__.zcol(i).z.size(); j++) {
                            /* global index of the G-vector */
                            size_t ig = gvec__.zcol_offs(i) + j;
                            #ifdef __CUFFT3D
                            auto G = gvec__.gvec().gvec(static_cast<int>(ig));
                            z_col_map_[ig] = grid_.index_by_gvec(G[0], G[1], G[2]);
                            #else
                            /* coordinate inside FFT 1D bufer */
                            int z = grid().coord_by_gvec(gvec__.zcol(i).z[j], 2);
                            z_col_map_[ig] = i * grid_.size(2) + z;
                            #endif
                        }
                    }
                    z_col_map_.copy_to_device();
                    
                    #ifdef __CUFFT3D
                    int dims_xy[] = {grid_.size(1), grid_.size(0)};
                    int dims[] = {grid_.size(2), grid_.size(1), grid_.size(0)};
                    work_size = std::max(cufft_get_work_size(2, dims_xy, cufft_nbatch_xy_),
                                         cufft_get_work_size(3, dims, 1));
                    #else
                    int dim_z[] = {grid_.size(2)};
                    cufft_nbatch_z_ = gvec__.num_zcol();
                    cufft_create_batch_plan(cufft_plan_z_, 1, dim_z, dim_z, 1, grid_.size(2), cufft_nbatch_z_, 0);

                    int dims_xy[] = {grid_.size(1), grid_.size(0)};
                    /* worksize for z and xy transforms */
                    work_size = std::max(cufft_get_work_size(2, dims_xy, cufft_nbatch_xy_),
                                         cufft_get_work_size(1, dim_z, cufft_nbatch_z_));
                    #endif
                } else {
                    int dims_xy[] = {grid_.size(0), grid_.size(1)};
                    work_size = cufft_get_work_size(2, dims_xy, cufft_nbatch_xy_);
                }
               
                /* allocate cufft work buffer */
                cufft_work_buf_ = mdarray<char, 1>(work_size, memory_t::device, "FFT3D.cufft_work_buf_");
                /* set work area for cufft */ 
                cufft_set_work_area(cufft_plan_xy_, cufft_work_buf_.at<GPU>());
                if (gpu_only_impl_) {
                    #ifdef __CUFFT3D
                    cufft_set_work_area(cufft_plan_3d_, cufft_work_buf_.at<GPU>());
                    #else
                    cufft_set_work_area(cufft_plan_z_, cufft_work_buf_.at<GPU>());
                    #endif
                }

                fft_buffer_aux1_.allocate(memory_t::device);
                fft_buffer_aux2_.allocate(memory_t::device);
                
                fft_buffer_.allocate(memory_t::device);

                z_col_pos_.allocate(memory_t::device);
                z_col_pos_.copy_to_device();
            }
            #endif

            prepared_ = true;
        }

        void dismiss()
        {
            #ifdef __GPU
            if (pu_ == GPU) {
                fft_buffer_aux1_.deallocate_on_device();
                fft_buffer_aux2_.deallocate_on_device();
                z_col_pos_.deallocate_on_device();
                fft_buffer_.deallocate_on_device();
                cufft_work_buf_.deallocate_on_device();
            }
            #endif
            prepared_ = false;
        }

        template <int direction>
        void transform(Gvec_partition const& gvec__, double_complex* data__)
        {
            PROFILE("sddk::FFT3D::transform");

            if (!prepared_) {
                TERMINATE("FFT3D is not ready");
            }

            /* reallocate auxiliary buffer if needed */
            size_t sz_max;
            if (comm_.size() > 1) {
                int rank = comm_.rank();
                int num_zcol_local = gvec__.zcol_distr_fft().counts[rank];
                /* we need this buffer size for mpi_alltoall */
                sz_max = std::max(grid_.size(2) * num_zcol_local, local_size());
            } else {
                sz_max = grid_.size(2) * gvec__.num_zcol();
            }
            if (sz_max > fft_buffer_aux1_.size()) {
                fft_buffer_aux1_ = mdarray<double_complex, 1>(sz_max, memory_t::host_pinned, "fft_buffer_aux1_");
                #ifdef __GPU
                if (pu_ == GPU) {
                    fft_buffer_aux1_.allocate(memory_t::device);
                }
                #endif
            }

            #ifdef __GPU
            if (comm_.size() == 1 && gpu_only_impl_ && cuda_check_device_ptr(data__)) {
                if (gvec__.reduced()) {
                    TERMINATE("not implemented");
                } else {
                    transform_3d_serial_gpu<direction, false>(gvec__, data__);
                }
                return;
            }
            #endif

            /* single node FFT */
            if (comm_.size() == 1) {
                switch (direction) {
                    case 1: {
                        if (gvec__.reduced()) {
                            transform_z_serial<1, true>(gvec__, data__, fft_buffer_aux1_);
                            transform_xy<1, true>(gvec__, fft_buffer_aux1_);
                        } else {
                            transform_z_serial<1, false>(gvec__, data__, fft_buffer_aux1_);
                            transform_xy<1, false>(gvec__, fft_buffer_aux1_);
                        }
                        break;
                    }
                    case -1: {
                        transform_xy<-1, false>(gvec__, fft_buffer_aux1_);
                        transform_z_serial<-1, false>(gvec__, data__, fft_buffer_aux1_);
                        break;
                    }
                    default: {
                        TERMINATE("wrong direction");
                    }
                }
            } else { /* parallel transform */
                switch (direction) {
                    case 1: {
                        if (gvec__.reduced()) {
                            transform_z_parallel<1, true>(gvec__, data__, fft_buffer_aux1_);
                            transform_xy<1, true>(gvec__, fft_buffer_aux1_);
                        } else {
                            transform_z_parallel<1, false>(gvec__, data__, fft_buffer_aux1_);
                            transform_xy<1, false>(gvec__, fft_buffer_aux1_);
                        }
                        break;
                    }
                    case -1: {
                        transform_xy<-1, false>(gvec__, fft_buffer_aux1_);
                        transform_z_parallel<-1, false>(gvec__, data__, fft_buffer_aux1_);
                        break;
                    }
                    default: {
                        TERMINATE("wrong direction");
                    }
                }   
            }
        }

        template <int direction>
        void transform(Gvec_partition const& gvec__, double_complex* data1__, double_complex* data2__)
        {
            PROFILE("sddk::FFT3D::transform");

            if (!prepared_) {
                TERMINATE("FFT3D is not ready");
            }

            if (!gvec__.reduced()) {
                TERMINATE("reduced set of G-vectors is required");
            }

            /* reallocate auxiliary buffers if needed */
            size_t sz_max;
            if (comm_.size() > 1) {
                int rank = comm_.rank();
                int num_zcol_local = gvec__.zcol_distr_fft().counts[rank];
                /* we need this buffer for mpi_alltoall */
                sz_max = std::max(grid_.size(2) * num_zcol_local, local_size());
            } else {
                sz_max = grid_.size(2) * gvec__.num_zcol();
            }
            
            if (sz_max > fft_buffer_aux1_.size()) {
                fft_buffer_aux1_ = mdarray<double_complex, 1>(sz_max, memory_t::host_pinned, "fft_buffer_aux1_");
                #ifdef __GPU
                if (pu_ == GPU) {
                    fft_buffer_aux1_.allocate(memory_t::device);
                }
                #endif
            }
            if (sz_max > fft_buffer_aux2_.size()) {
                fft_buffer_aux2_ = mdarray<double_complex, 1>(sz_max, memory_t::host_pinned, "fft_buffer_aux2_");
                #ifdef __GPU
                if (pu_ == GPU) {
                    fft_buffer_aux2_.allocate(memory_t::device);
                }
                #endif
            }

            /* single node FFT */
            if (comm_.size() == 1) {
                switch (direction) {
                    case 1: {
                        transform_z_serial<1, true>(gvec__, data1__, fft_buffer_aux1_);
                        transform_z_serial<1, true>(gvec__, data2__, fft_buffer_aux2_);
                        transform_xy<1>(gvec__, fft_buffer_aux1_, fft_buffer_aux2_);
                        break;
                    }
                    case -1: {
                        transform_xy<-1>(gvec__, fft_buffer_aux1_, fft_buffer_aux2_);
                        transform_z_serial<-1, false>(gvec__, data1__, fft_buffer_aux1_);
                        transform_z_serial<-1, false>(gvec__, data2__, fft_buffer_aux2_);
                        break;
                    }
                    default: {
                        TERMINATE("wrong direction");
                    }
                }
            } else { /* parallel transform */
                switch (direction) {
                    case 1: {
                        transform_z_parallel<1, true>(gvec__, data1__, fft_buffer_aux1_);
                        transform_z_parallel<1, true>(gvec__, data2__, fft_buffer_aux2_);
                        transform_xy<1>(gvec__, fft_buffer_aux1_, fft_buffer_aux2_);
                        break;
                    }
                    case -1: {
                        transform_xy<-1>(gvec__, fft_buffer_aux1_, fft_buffer_aux2_);
                        transform_z_parallel<-1, false>(gvec__, data1__, fft_buffer_aux1_);
                        transform_z_parallel<-1, false>(gvec__, data2__, fft_buffer_aux2_);
                        break;
                    }
                    default: {
                        TERMINATE("wrong direction");
                    }
                }   
            }
        }

        #ifdef __GPU
        void copy_to_device()
        {
            fft_buffer_.copy_to_device();
        }
        #endif
};

} // namespace sddk

#endif // __FFT3D_H__

/** \page ft_pw Fourier transform and plane wave normalization
 *
 *  We use plane waves in two different cases: a) plane waves (or augmented plane waves in the case of APW+lo method)
 *  form a basis for expanding Kohn-Sham wave functions and b) plane waves are used to expand charge density and
 *  potential. When we are dealing with plane wave basis functions it is convenient to adopt the following 
 *  normalization:
 *  \f[
 *      \langle {\bf r} |{\bf G+k} \rangle = \frac{1}{\sqrt \Omega} e^{i{\bf (G+k)r}}
 *  \f]
 *  such that
 *  \f[
 *      \langle {\bf G+k} |{\bf G'+k} \rangle_{\Omega} = \delta_{{\bf GG'}}
 *  \f]
 *  in the unit cell. However, for the expansion of periodic functions such as density or potential, the following 
 *  convention is more appropriate:
 *  \f[
 *      \rho({\bf r}) = \sum_{\bf G} e^{i{\bf Gr}} \rho({\bf G})
 *  \f]
 *  where
 *  \f[
 *      \rho({\bf G}) = \frac{1}{\Omega} \int_{\Omega} e^{-i{\bf Gr}} \rho({\bf r}) d{\bf r} = 
 *          \frac{1}{\Omega} \sum_{{\bf r}_i} e^{-i{\bf Gr}_i} \rho({\bf r}_i) \frac{\Omega}{N} = 
 *          \frac{1}{N} \sum_{{\bf r}_i} e^{-i{\bf Gr}_i} \rho({\bf r}_i) 
 *  \f]
 *  i.e. with such convention the plane-wave expansion coefficients are obtained with a normalized FFT.
 */
