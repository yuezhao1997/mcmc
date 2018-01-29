/*################################################################################
  ##
  ##   Copyright (C) 2011-2018 Keith O'Hara
  ##
  ##   This file is part of the MCMC C++ library.
  ##
  ##   MCMC is free software: you can redistribute it and/or modify
  ##   it under the terms of the GNU General Public License as published by
  ##   the Free Software Foundation, either version 2 of the License, or
  ##   (at your option) any later version.
  ##
  ##   MCMC is distributed in the hope that it will be useful,
  ##   but WITHOUT ANY WARRANTY; without even the implied warranty of
  ##   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  ##   GNU General Public License for more details.
  ##
  ################################################################################*/
 
/*
 * Riemannian Manifold Hamiltonian Monte Carlo (RM-HMC)
 */

#include "mcmc.hpp" 

bool
mcmc::rmhmc_int(const arma::vec& initial_vals, arma::mat& draws_out, std::function<double (const arma::vec& vals_inp, arma::vec* grad_out, void* target_data)> target_log_kernel, void* target_data, std::function<arma::mat (const arma::vec& vals_inp, arma::cube* tensor_deriv_out, void* tensor_data)> tensor_fn, void* tensor_data, algo_settings* settings_inp)
{
    bool success = false;

    const double BIG_NEG_VAL = MCMC_BIG_NEG_VAL;
    const int n_vals = initial_vals.n_elem;

    //
    // settings

    algo_settings settings;

    if (settings_inp) {
        settings = *settings_inp;
    }

    const int n_draws_keep   = settings.hmc_n_draws;
    const int n_draws_burnin = settings.hmc_n_burnin;

    const double step_size = settings.hmc_step_size;
    const int n_leap_steps = settings.hmc_leap_steps;
    const int n_fp_steps = settings.rmhmc_fp_steps;

    const bool vals_bound = settings.vals_bound;
    
    const arma::vec lower_bounds = settings.lower_bounds;
    const arma::vec upper_bounds = settings.upper_bounds;

    const arma::uvec bounds_type = determine_bounds_type(vals_bound, n_vals, lower_bounds, upper_bounds);
    
    //
    // lambda function for box constraints

    std::function<double (const arma::vec& vals_inp, arma::vec* grad_out, void* box_data)> box_log_kernel = [target_log_kernel, vals_bound, bounds_type, lower_bounds, upper_bounds] (const arma::vec& vals_inp, arma::vec* grad_out, void* target_data) -> double {
        //
        if (vals_bound) {
            arma::vec vals_inv_trans = inv_transform(vals_inp, bounds_type, lower_bounds, upper_bounds);

            return target_log_kernel(vals_inv_trans, nullptr, target_data) + log_jacobian(vals_inp, bounds_type, lower_bounds, upper_bounds);
        } else {
            return target_log_kernel(vals_inp, nullptr, target_data);
        }
    };

    // momentum update

    std::function<arma::vec (const arma::vec& pos_inp, const arma::vec& mntm_inp, void* target_data, const double step_size, const arma::mat& inv_tensor_mat, const arma::cube& tensor_deriv, arma::mat* jacob_matrix_out)> mntm_update_fn \
    = [target_log_kernel, vals_bound, bounds_type, lower_bounds, upper_bounds] (const arma::vec& pos_inp, const arma::vec& mntm_inp, void* target_data, const double step_size, const arma::mat& inv_tensor_mat, const arma::cube& tensor_deriv, arma::mat* jacob_matrix_out) \
    -> arma::vec 
    {
        const int n_vals = pos_inp.n_elem;
        arma::vec grad_obj(n_vals);

        if (vals_bound)
        {
            arma::vec pos_inv_trans = inv_transform(pos_inp, bounds_type, lower_bounds, upper_bounds);
            target_log_kernel(pos_inv_trans,&grad_obj,target_data);

            //

            for (int i=0; i < n_vals; i++) {
                arma::mat tmp_mat = inv_tensor_mat * tensor_deriv.slice(i);

                grad_obj(i) = - grad_obj(i) + 0.5*( arma::trace(tmp_mat) - arma::as_scalar(mntm_inp.t() * tmp_mat * inv_tensor_mat * mntm_inp) );
            }

            //

            arma::mat jacob_matrix = inv_jacobian_adjust(pos_inp,bounds_type,lower_bounds,upper_bounds);

            if (jacob_matrix_out) {
                *jacob_matrix_out = jacob_matrix;
            }

            //

            return step_size * jacob_matrix * grad_obj / 2.0;
        } 
        else
        {
            target_log_kernel(pos_inp,&grad_obj,target_data);

            //

            for (int i=0; i < n_vals; i++) {
                arma::mat tmp_mat = inv_tensor_mat * tensor_deriv.slice(i);

                grad_obj(i) = - grad_obj(i) + 0.5*( arma::trace(tmp_mat) - arma::as_scalar(mntm_inp.t() * tmp_mat * inv_tensor_mat * mntm_inp) );
            }

            //

            arma::vec mntm_out = step_size * grad_obj / 2.0;

            return step_size * grad_obj / 2.0;
        }
    };

    std::function<arma::mat (const arma::vec& vals_inp, arma::cube* tensor_deriv_out, void* tensor_data)> box_tensor_fn \
    = [tensor_fn, vals_bound, bounds_type, lower_bounds, upper_bounds] (const arma::vec& vals_inp, arma::cube* tensor_deriv_out, void* tensor_data) \
    -> arma::mat
    {
        if (vals_bound)
        {
            arma::vec vals_inv_trans = inv_transform(vals_inp, bounds_type, lower_bounds, upper_bounds);

            return tensor_fn(vals_inv_trans, tensor_deriv_out, tensor_data);
        }
        else
        {
            return tensor_fn(vals_inp, tensor_deriv_out, tensor_data);
        }
    };

    //
    // setup
    
    arma::vec first_draw = initial_vals;

    if (vals_bound){ // should we transform the parameters?
        first_draw = transform(initial_vals, bounds_type, lower_bounds, upper_bounds);
    }

    draws_out.set_size(n_draws_keep, n_vals);
    
    arma::vec prev_draw = first_draw;
    arma::vec new_draw  = first_draw;

    arma::vec new_mntm  = arma::randn(n_vals,1);

    arma::cube new_deriv_cube;
    arma::mat new_tensor = box_tensor_fn(new_draw,&new_deriv_cube,tensor_data);

    arma::mat prev_tensor = new_tensor;
    arma::mat inv_new_tensor = arma::inv(new_tensor);
    arma::mat inv_prev_tensor = inv_new_tensor;

    arma::cube prev_deriv_cube = new_deriv_cube;

    const double cons_term = 0.5*n_vals*std::log( 2.0 * arma::datum::pi );

    double prev_U = cons_term - box_log_kernel(first_draw, nullptr, target_data) + std::log(arma::det(new_tensor));
    double prop_U = prev_U;

    double prop_K, prev_K;

    //
    // loop

    int n_accept = 0;    
    double comp_val;
    
    for (int jj = 0; jj < n_draws_keep + n_draws_burnin; jj++) {

        new_mntm = arma::chol(prev_tensor,"lower")*arma::randn(n_vals,1);
        prev_K = arma::dot(new_mntm,inv_prev_tensor*new_mntm) / 2.0;

        new_draw = prev_draw;

        for (int k = 0; k < n_leap_steps; k++)
        {   // begin leap frog steps

            arma::vec prop_mntm = new_mntm;

            for (int kk=0; kk < n_fp_steps; kk++) {
                prop_mntm = new_mntm + mntm_update_fn(new_draw,prop_mntm,target_data,step_size,inv_prev_tensor,prev_deriv_cube,nullptr); // half-step
            }

            new_mntm = prop_mntm;

            //

            arma::vec prop_draw = new_draw;
            // new_draw += step_size * inv_prev_tensor * new_mntm;

            for (int kk=0; kk < n_fp_steps; kk++) {
                inv_new_tensor = arma::inv(box_tensor_fn(prop_draw,nullptr,tensor_data));

                prop_draw = new_draw + 0.5 * step_size * ( inv_prev_tensor + inv_new_tensor ) * new_mntm;
            }

            new_draw = prop_draw;

            new_tensor = box_tensor_fn(new_draw,&new_deriv_cube,tensor_data);
            inv_new_tensor = arma::inv(new_tensor);

            //

            new_mntm += mntm_update_fn(new_draw,new_mntm,target_data,step_size,inv_new_tensor,new_deriv_cube,nullptr); // half-step

        }

        prop_U = cons_term - box_log_kernel(new_draw, nullptr, target_data) + 0.5*std::log(arma::det(new_tensor));
        
        if (!std::isfinite(prop_U)) {
            prop_U = -BIG_NEG_VAL;
        }

        prop_K = arma::dot(new_mntm,inv_new_tensor*new_mntm) / 2.0;

        //

        comp_val = std::min(0.0, - (prop_U + prop_K) + (prev_U + prev_K));
        double z_rand = arma::as_scalar(arma::randu(1));

        if (z_rand < std::exp(comp_val)) 
        {
            prev_draw = new_draw;

            prev_U = prop_U;
            prev_K = prop_K;

            prev_tensor     = new_tensor;
            inv_prev_tensor = inv_new_tensor;
            prev_deriv_cube = new_deriv_cube;

            if (jj >= n_draws_burnin) {
                draws_out.row(jj - n_draws_burnin) = new_draw.t();
                n_accept++;
            }
        }
        else
        {
            if (jj >= n_draws_burnin) {
                draws_out.row(jj - n_draws_burnin) = prev_draw.t();
            }
        }
    }

    success = true;

    //

    if (vals_bound) {
#ifdef MCMC_USE_OMP
        #pragma omp parallel for
#endif
        for (int jj = 0; jj < n_draws_keep; jj++) {
            draws_out.row(jj) = arma::trans(inv_transform(draws_out.row(jj).t(), bounds_type, lower_bounds, upper_bounds));
        }
    }

    if (settings_inp) {
        settings_inp->hmc_accept_rate = (double) n_accept / (double) n_draws_keep;
    }

    //

    return success;
}

// wrappers

bool
mcmc::rmhmc(const arma::vec& initial_vals, arma::mat& draws_out, std::function<double (const arma::vec& vals_inp, arma::vec* grad_out, void* target_data)> target_log_kernel, void* target_data, std::function<arma::mat (const arma::vec& vals_inp, arma::cube* tensor_deriv_out, void* tensor_data)> tensor_fn, void* tensor_data)
{
    return rmhmc_int(initial_vals,draws_out,target_log_kernel,target_data,tensor_fn,tensor_data,nullptr);
}

bool
mcmc::rmhmc(const arma::vec& initial_vals, arma::mat& draws_out, std::function<double (const arma::vec& vals_inp, arma::vec* grad_out, void* target_data)> target_log_kernel, void* target_data, std::function<arma::mat (const arma::vec& vals_inp, arma::cube* tensor_deriv_out, void* tensor_data)> tensor_fn, void* tensor_data, algo_settings& settings)
{
    return rmhmc_int(initial_vals,draws_out,target_log_kernel,target_data,tensor_fn,tensor_data,&settings);
}