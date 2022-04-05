#include <stdio.h>
#include <math.h>
#include <limits.h>
#include <complex.h>
#include <time.h>
#ifdef _OPENMP
#   include <omp.h>
#endif

#include <fftw3.h>

#include <gsl/gsl_math.h>
#include <gsl/gsl_rng.h>
#include <gsl/gsl_randist.h>
#include <gsl/gsl_histogram.h>

#include "configs.h"
#include "utils.h"
#include "object.h"
#include "filter.h"
#include "profiles.h"
#include "onepoint.h"
#include "maps.h"

#include "hmpdf.h"

int
null_maps(hmpdf_obj *d)
{//{{{
    STARTFCT

    d->m->created_sidelengths = 0;

    d->m->created_mem = 0;

    d->m->created_ellgrid = 0;
    d->m->ellgrid = NULL;

    d->m->created_map = 0;
    d->m->map_real = NULL;
    d->m->p_r2c = NULL;
    d->m->p_c2r = NULL;

    d->m->created_map_ws = 0;
    d->m->ws = NULL;

    ENDFCT
}//}}}

int
reset_maps(hmpdf_obj *d)
{//{{{
    STARTFCT

    HMPDFPRINT(2, "\treset_maps\n");

    if (d->m->ellgrid != NULL) { free(d->m->ellgrid); }
    if (d->m->map_real != NULL)
    {
        if (d->m->need_ft)
        {
            fftw_free(d->m->map_real);
        }
        else
        {
            free(d->m->map_real);
        }
    }
    if (d->m->p_r2c != NULL) { fftw_destroy_plan(*(d->m->p_r2c)); free(d->m->p_r2c); }
    if (d->m->p_c2r != NULL) { fftw_destroy_plan(*(d->m->p_c2r)); free(d->m->p_c2r); }
    if (d->m->ws != NULL)
    {
        for (int ii=0; ii<d->m->Nws; ii++)
        {
            if (d->m->ws[ii] != NULL)
            {
                if (d->m->ws[ii]->map != NULL)
                {
                    if (d->m->ws[ii]->for_fft)
                    {
                        fftw_free(d->m->ws[ii]->map);
                    }
                    else
                    {
                        free(d->m->ws[ii]->map);
                    }
                }
                if (d->m->ws[ii]->pos != NULL) { free(d->m->ws[ii]->pos); }
                if (d->m->ws[ii]->buf != NULL) { free(d->m->ws[ii]->buf); }
                if (d->m->ws[ii]->rng != NULL) { gsl_rng_free(d->m->ws[ii]->rng); }
                if (d->m->ws[ii]->p_r2c != NULL)
                {
                    fftw_destroy_plan(*(d->m->ws[ii]->p_r2c));
                    free(d->m->ws[ii]->p_r2c);
                }
                free(d->m->ws[ii]);
            }
        }
        free(d->m->ws);
    }

    ENDFCT
}//}}}

#define NEWMAPWS_SAFEALLOC(var, expr)  \
    do {                               \
        var = expr;                    \
        if (UNLIKELY(!(var)))          \
        {                              \
            if (ws->map != NULL)       \
            { free(ws->map); }         \
            if (ws->pos != NULL)       \
            { free(ws->pos); }         \
            if (ws->buf != NULL)       \
            { free(ws->buf); }         \
            free(*out);                \
            if (ws->rng != NULL)       \
            { gsl_rng_free(ws->rng); } \
            return 1;                  \
        }                              \
    } while (0)

static int
new_map_ws(hmpdf_obj *d, int idx, map_ws **out)
// allocates a new map workspace and creates fft if necessary
{//{{{
    STARTFCT

    SAFEALLOC(*out, malloc(sizeof(map_ws)));

    map_ws *ws = *out; // for convenience

    // initialize to NULL so we can free realiably in case an alloc fails
    ws->map = NULL;
    ws->pos = NULL;
    ws->buf = NULL;
    ws->rng = NULL;
    ws->p_r2c = NULL;

    if (idx == 0)
    {
        ws->for_fft = 1;
        ws->ldmap = d->m->Nside+2;
    }
    else
    {
        ws->for_fft = 0;
        ws->ldmap = d->m->Nside;
    }

    NEWMAPWS_SAFEALLOC(ws->pos, malloc(d->m->buflen
                                       * sizeof(double)));
    NEWMAPWS_SAFEALLOC(ws->buf, malloc(d->m->buflen
                                       * sizeof(double)));

    // the performance of the random number generator can actually
    //     turn out to be a bottle neck, so we use a relatively
    //     fast one
    NEWMAPWS_SAFEALLOC(ws->rng, gsl_rng_alloc(gsl_rng_taus));

    NEWMAPWS_SAFEALLOC(ws->map, ((ws->for_fft) ?
                                 fftw_malloc
                                 : malloc)(ws->ldmap * d->m->Nside
                                           * sizeof(double)));

    if (ws->for_fft)
    {
        ws->map_comp = (double complex *)ws->map;
        if (d->f->has_z_dependent)
        {
            NEWMAPWS_SAFEALLOC(ws->p_r2c, malloc(sizeof(fftw_plan)));
            *(ws->p_r2c) = fftw_plan_dft_r2c_2d(d->m->Nside, d->m->Nside,
                                                ws->map, ws->map_comp, FFTW_MEASURE);
        }
    }

    ENDFCT
}//}}}

#undef NEWMAPWS_SAFEALLOC

static int
create_map_ws(hmpdf_obj *d)
// allocates as many workspaces as necessary/possible
{//{{{
    STARTFCT

    if (d->m->created_map_ws) { return 0; }

    HMPDFPRINT(2, "\tcreate_map_ws\n");
    HMPDFPRINT(3, "\t\ttrying to allocate workspaces for %d threads.\n", d->Ncores);

    SAFEALLOC(d->m->ws, malloc(d->Ncores * sizeof(map_ws *)));
    SETARRNULL(d->m->ws, d->Ncores);
    d->m->Nws = 0;
    for (int ii=0; ii<d->Ncores; ii++)
    {
        int alloc_failed = new_map_ws(d, ii, d->m->ws+ii);
        if (alloc_failed)
        {
            d->m->ws[ii] = NULL;
            break;
        }
        else
        {
            ++d->m->Nws;
        }
    }

    if (d->m->Nws < d->Ncores)
    {
        HMPDFPRINT(1, "Allocated only %d workspaces, "
                      "because memory ran out.\n", d->m->Nws);
    }

    HMPDFCHECK(d->m->Nws<1, "Failed to allocate any workspaces.");

    d->m->created_map_ws = 1;

    ENDFCT
}//}}}

static int
create_ellgrid(hmpdf_obj *d)
{//{{{
    STARTFCT

    if (d->m->created_ellgrid) { return 0; }

    HMPDFPRINT(2, "\tcreate_ellgrid\n");

    SAFEALLOC(d->m->ellgrid, malloc((d->m->Nside/2+1) * sizeof(double)));
    SAFEHMPDF(linspace(d->m->Nside/2+1,
                       0.0, M_PI/d->f->pixelside,
                       d->m->ellgrid));

    d->m->created_ellgrid = 1;

    ENDFCT
}//}}}

static int
reset_map_ws(hmpdf_obj *d, map_ws *ws)
{//{{{
    STARTFCT

    unsigned long seed;
    if (d->m->mapseed == INT_MAX)
    // we regard this as seed not set
    //     the case where the seed assumes this value
    //     appears quite unlikely,
    //     and even that would not be fatal
    //     since on the next iteration the calling
    //     code would probably use a different one
    {
        seed = (unsigned long)(time(NULL))
               + (unsigned long)(clock())
               + (unsigned long)(rand())
               + (unsigned long)(ws->buf);
    }
    else
    // the calling code has set a seed
    //     we called srand() with this seed
    //     so now we're getting reproducible
    //     random numbers
    {
        seed = (unsigned long)(rand());
    }

    // seed the random number generator
    gsl_rng_set(ws->rng, seed);

    zero_real(ws->ldmap * d->m->Nside, ws->map);

    ENDFCT
}//}}}

static int
fill_buf(hmpdf_obj *d, int z_index, int M_index, map_ws *ws)
// creates a map of the given object in the buffer
{//{{{
    STARTFCT

    // theta_out in units of the pixel spacing
    double tout = d->p->profiles[z_index][M_index][0]
                  / d->f->pixelside;

    // compute how large this specific map needs to be
    //     the map is of size (2*w+1)^2
    long w = (long)ceil(tout);
    ws->bufside = 2 * w + 1;
    long pixside = 2 * d->m->pxlgrid + 1;

    // draw random displacement of the center of the halo
    double dx = gsl_rng_uniform(ws->rng) - 0.5;
    double dy = gsl_rng_uniform(ws->rng) - 0.5;

    long Npix_filled = 0;
    while (Npix_filled < ws->bufside * ws->bufside)
    {
        long Npix_here
            = GSL_MIN(ws->bufside * ws->bufside - Npix_filled,           // physical constraint
                      (d->m->buflen - Npix_filled) / (pixside*pixside)); // memory constraint
        HMPDFCHECK(Npix_here <= 0, "no buffer left. this is a bug.");
        
        long posidx = 0;
        // fill the position buffer
        for (long ii=0; ii<Npix_here; ii++)
        {
            // figure out pixel coordinates in the map
            long xx = (ii + Npix_filled) / ws->bufside - w;
            long yy = (ii + Npix_filled) % ws->bufside - w;

            // loop over sample points within the pixel
            for (long xp= -d->m->pxlgrid; xp<= d->m->pxlgrid; xp++)
            {
                for (long yp= -d->m->pxlgrid; yp<= d->m->pxlgrid; yp++, posidx++)
                {
                    double xpos = (double)xx + (double)(2*xp)/(double)pixside + dx;
                    double ypos = (double)yy + (double)(2*yp)/(double)pixside + dy;
                    ws->pos[posidx] = hypot(xpos, ypos) / tout;
                }
            }
        }

        // evaluate the profile interpolator
        SAFEHMPDF(s_of_t(d, z_index, M_index, posidx, ws->pos, ws->buf+Npix_filled));

        posidx = 0;
        // perform the average
        for (long ii=0; ii<Npix_here; ii++)
        {
            double temp = 0.0;
            for (long jj=0; jj<pixside*pixside; jj++, posidx++)
            {
                temp += ws->buf[Npix_filled + posidx];
            }
            temp /= (double)(pixside*pixside);
            ws->buf[Npix_filled + ii] = temp;
        }

        Npix_filled +=  Npix_here;
    }

    ENDFCT
}//}}}

// convenience macro to reduce typing
#define INNERLOOP_OP                      \
    ws->map[ixx*ws->ldmap + iyy]          \
        += ws->buf[xx * ws->bufside + yy];

static inline void 
add_buf_inner_loop(hmpdf_obj *d, map_ws *ws, long y0, long xx, long ixx)
{//{{{
    for (long yy=0, iyy=y0;
         yy< GSL_MIN(ws->bufside, d->m->Nside - y0);
         yy++, iyy++)
    {
        INNERLOOP_OP
    }
    for (long yy= GSL_MIN(ws->bufside, d->m->Nside - y0), iyy=0;
         UNLIKELY(yy< ws->bufside);
         yy++, iyy++)
    {
        INNERLOOP_OP
    }
}//}}}

#define OUTERLOOP_OP \
    add_buf_inner_loop(d, ws, y0, xx, ixx);

static int 
add_buf(hmpdf_obj *d, map_ws *ws)
// picks random position in the map and adds buffer map once,
//     satisfies periodic boundary conditions
{//{{{
    STARTFCT

    // pick a random point in the map
    long x0 = gsl_rng_uniform_int(ws->rng, d->m->Nside);
    long y0 = gsl_rng_uniform_int(ws->rng, d->m->Nside);

    // add the pixel values from the buffer
    //     we 'unroll' the loops slightly for better efficiency
    //     with the periodic boundary conditions
    // A word on notation : xx, yy are coordinates in this map
    //                             (the one stored in ws->buf)
    //                      ixx, iyy are coordinates in the total map
    //                             (the one stored in ws->map)
    for (long xx=0, ixx=x0;
         xx< GSL_MIN(ws->bufside, d->m->Nside - x0);
         xx++, ixx++)
    {
        OUTERLOOP_OP
    }
    for (long xx= GSL_MIN(ws->bufside, d->m->Nside - x0), ixx=0;
         UNLIKELY(xx< ws->bufside);
         xx++, ixx++)
    {
        OUTERLOOP_OP
    }

    ENDFCT
}//}}}

#undef OUTERLOOP_OP
#undef INNERLOOP_OP

static int
draw_N_halos(hmpdf_obj *d, int z_index, int M_index, map_ws *ws, unsigned *N)
// draws the number of halos in the given bin
{//{{{
    STARTFCT

    // expected number of halos in this bin
    double n = d->h->hmf[z_index][M_index] // dn_3d / dlogM
               * gsl_pow_2(d->c->comoving[z_index])
               / d->c->hubble[z_index]
               * d->n->zweights[z_index]
               * d->n->Mweights[M_index]
               * d->m->area;

    if (d->m->mappoisson)
    {
        // use this funny construct because this function
        //    frequently sets errno (due to underflows I think)
        //    which is not critical but would be caught
        //    by ENDFCT
        SAFEGSL((*N = gsl_ran_poisson(ws->rng, n), 0));
    }
    else
    {
        double r = gsl_rng_uniform(ws->rng);
        double w_ceil = n - floor(n);
        *N = (unsigned)round((r < w_ceil) ? ceil(n) : floor(n));
    }

    ENDFCT
}//}}}

static int
do_this_bin(hmpdf_obj *d, int z_index, int M_index, map_ws *ws)
// draws random integer from correct distribution
// if ==0, return
// else, fill_buf and then integer x add_buf
{//{{{
    STARTFCT

    unsigned N;
    SAFEHMPDF(draw_N_halos(d, z_index, M_index, ws, &N));

    if (N == 0)
    {
        return 0;
    }
    else
    {
        SAFEHMPDF(fill_buf(d, z_index, M_index, ws));

        // FIXME this is a quick fix to deal with small HSC maps
        if (ws->bufside >= d->m->Nside)
            return 0;

        HMPDFCHECK(ws->bufside >= d->m->Nside,
                   "attempting to add a halo that is larger than the map. "
                   "You should make the map larger.");

        for (unsigned ii=0; ii<N; ii++)
        {
            SAFEHMPDF(add_buf(d, ws));
        }
    }

    ENDFCT
}//}}}

static int
add_grf(hmpdf_obj *d, double (*pwr_spec)(double, void *), void *pwr_spec_params)
// adds random GRF realization to the Fourier space map
// if pwr_spec == NULL, no computation is performed
{//{{{
    STARTFCT

    if (pwr_spec == NULL)
    {
        return 0;
    }
    else
    {
        HMPDFPRINT(2, "\tadding Gaussian random field to the map\n");

        #ifdef _OPENMP
        #   pragma omp parallel for num_threads(d->m->Nws) schedule(static)
        #endif
        for (long ii=0; ii<d->m->Nside; ii++)
        // loop over the long direction (rows)
        {
            double ell1 = WAVENR(d->m->Nside, d->m->ellgrid, ii);

            for (long jj=0; jj<d->m->Nside/2+1; jj++)
            {
                CONTINUE_IF_ERR

                double ell2 = WAVENR(d->m->Nside, d->m->ellgrid, jj);
                double ellmod = hypot(ell1, ell2);

                double Cl = pwr_spec(ellmod, pwr_spec_params);

                HMPDFCHECK_NORETURN(Cl < 0.0,
                                    "power spectrum must be positive everywhere.");
                
                CONTINUE_IF_ERR

                double complex ampl
                    = gsl_ran_gaussian(d->m->ws[THIS_THREAD]->rng, 1.0)
                      + _Complex_I * gsl_ran_gaussian(d->m->ws[THIS_THREAD]->rng, 1.0);
                ampl *= sqrt(0.5 * Cl) / d->f->pixelside
                        * (double)(d->m->Nside);

                d->m->map_comp[ii*(d->m->Nside/2+1)+jj] += ampl;
            }
        }
    }

    ENDFCT
}//}}}

static int
filter_map(hmpdf_obj *d, double complex *map_comp, int *z_index)
{//{{{
    STARTFCT

    if (z_index == NULL)
    {
        HMPDFPRINT(3, "\t\tapplying filters to the map\n");
    }

    #ifdef _OPENMP
    #   pragma omp parallel for num_threads(d->Ncores) schedule(static)
    #endif
    for (long ii=0; ii<d->m->Nside; ii++)
    // loop over long direction (rows)
    {
        CONTINUE_IF_ERR

        double *ellmod;
        SAFEALLOC_NORETURN(ellmod, malloc((d->m->Nside/2+1) * sizeof(double)));

        CONTINUE_IF_ERR

        double ell1 = WAVENR(d->m->Nside, d->m->ellgrid, ii);

        for (long jj=0; jj<d->m->Nside/2+1; jj++)
        // loop over short direction (cols)
        {
            double ell2 = WAVENR(d->m->Nside, d->m->ellgrid, jj);
            ellmod[jj] = hypot(ell1, ell2);
        }

        SAFEHMPDF_NORETURN(apply_filters_map(d, d->m->Nside/2+1, ellmod,
                                             map_comp + ii * (d->m->Nside/2+1),
                                             map_comp + ii * (d->m->Nside/2+1),
                                             z_index));

        free(ellmod);
    }


    ENDFCT
}//}}}

static int
loop_no_z_dependence(hmpdf_obj *d)
// the loop if there are no z-dependent filters
// NOTE : the map that comes out of this is in conjugate space
//        if d->m->need_ft
{//{{{
    STARTFCT

    HMPDFPRINT(3, "\t\tloop_no_z_dependence\n");

    // reset the workspaces
    for (int ii=0; ii<d->m->Nws; ii++)
    {
        SAFEHMPDF(reset_map_ws(d, d->m->ws[ii]));
    }

    // create the array of bins
    int *bins;
    SAFEALLOC(bins, malloc(d->n->Nz * d->n->NM * sizeof(int)));
    for (int ii=0; ii<d->n->Nz * d->n->NM; ii++)
    {
        bins[ii] = ii;
    }
    // shuffle to equalize load
    gsl_ran_shuffle(d->m->ws[0]->rng, bins, d->n->Nz * d->n->NM, sizeof(int));

    // status
    int Nstatus = 0;
    time_t start_time = time(NULL);

    // perform the loop
    #ifdef _OPENMP
    #   pragma omp parallel for num_threads(d->m->Nws) schedule(dynamic)
    #endif
    for (int ii=0; ii<d->n->Nz * d->n->NM; ii++)
    {
        CONTINUE_IF_ERR

        int z_index = bins[ii] / d->n->NM;
        int M_index = bins[ii] % d->n->NM;
        SAFEHMPDF_NORETURN(do_this_bin(d, z_index, M_index,
                                       d->m->ws[THIS_THREAD]));

        #ifdef _OPENMP
        #   pragma omp critical(StatusMapNoz)
        #endif
        {
            ++Nstatus;
            if ((Nstatus%MAPNOZ_STATUS_PERIOD == 0) && (d->verbosity > 0))
            {
                TIMEREMAIN(Nstatus, d->n->Nz * d->n->NM, "create_map");
            }
        }
    }

    TIMEELAPSED("create_map");

    free(bins);

    // add to the total map
    for (int ii=0; ii<d->m->Nws; ii++)
    {
        #ifdef _OPENMP
        #   pragma omp parallel for num_threads(d->Ncores) schedule(static)
        #endif
        for (long jj=0; jj<d->m->Nside; jj++)
        {
            for (long kk=0; kk<d->m->Nside; kk++)
            {
                d->m->map_real[jj*d->m->ldmap + kk]
                    += d->m->ws[ii]->map[jj*d->m->Nside+kk];
            }
        }
    }

    if (d->m->need_ft)
    {
        // transform to conjugate space
        HMPDFCHECK(d->m->p_r2c == NULL,
                   "trying to execute an fftw_plan that has not been initialized.");
        fftw_execute(*(d->m->p_r2c));
    }

    ENDFCT
}//}}}

static int
loop_w_z_dependence(hmpdf_obj *d)
// the less efficient loop we need to perform if there is a z-dependent filter
// NOTE : the map that comes out of this is in conjugate space!
{//{{{
    STARTFCT

    HMPDFPRINT(3, "\t\tloop_w_z_dependence\n");

    int *zbins;
    SAFEALLOC(zbins, malloc(d->n->Nz * sizeof(int)));
    for (int ii=0; ii<d->n->Nz; ii++)
    {
        zbins[ii] = ii;
    }
    // shuffle for more representative status updates
    gsl_ran_shuffle(d->m->ws[0]->rng, zbins, d->n->Nz, sizeof(int));

    int *Mbins;
    SAFEALLOC(Mbins, malloc(d->n->NM * sizeof(int)));
    for (int ii=0; ii<d->n->NM; ii++)
    {
        Mbins[ii] = ii;
    }

    // status
    time_t start_time = time(NULL);

    for (int zz=0; zz<d->n->Nz; zz++)
    {
        int z_index = zbins[zz];

        // reset the workspaces
        for (int ii=0; ii<d->m->Nws; ii++)
        {
            SAFEHMPDF(reset_map_ws(d, d->m->ws[ii]));
        }

        // shuffle to equalize load
        gsl_ran_shuffle(d->m->ws[0]->rng, Mbins, d->n->NM, sizeof(int));

        #ifdef _OPENMP
        #   pragma omp parallel for num_threads(d->m->Nws) schedule(dynamic)
        #endif
        for (int mm=0; mm<d->n->NM; mm++)
        {
            CONTINUE_IF_ERR
            int M_index = Mbins[mm];
            SAFEHMPDF_NORETURN(do_this_bin(d, z_index, M_index,
                                           d->m->ws[THIS_THREAD]));
        }

        // sum all sub-maps in the 0th one (which always exists)
        for (int ii=1; ii<d->m->Nws; ii++)
        {
            for (long jj=0; jj<d->m->Nside; jj++)
            {
                for (long kk=0; kk<d->m->Nside; kk++)
                {
                    d->m->ws[0]->map[jj*(d->m->Nside+2) + kk]
                        += d->m->ws[ii]->map[jj*d->m->Nside + kk];
                }
            }
        }

        // transform to conjugate space
        HMPDFCHECK(d->m->ws[0]->p_r2c == NULL,
                   "trying to execute an fftw_plan that has not been initialized.");
        fftw_execute(*(d->m->ws[0]->p_r2c));

        // apply the z-dependent filters
        SAFEHMPDF(filter_map(d, d->m->ws[0]->map_comp, &z_index));

        // add to the total map
        for (long ii=0; ii<d->m->Nside * (d->m->Nside/2+1); ii++)
        {
            d->m->map_comp[ii] += d->m->ws[0]->map_comp[ii];
        }

        if (((zz+1)%MAPWZ_STATUS_PERIOD == 0) && (d->verbosity > 0))
        {
            TIMEREMAIN(zz+1, d->n->Nz, "create_map");
        }
    }

    TIMEELAPSED("create_map");

    free(Mbins);
    free(zbins);

    ENDFCT
}//}}}

static int
create_mem(hmpdf_obj *d)
{//{{{
    STARTFCT

    if (d->m->created_mem) { return 0; }

    HMPDFPRINT(2, "\tcreate_mem\n");

    if (d->f->Nfilters > 1 // the pixelization is done in real space,
                           //     which is more accurate
        || d->ns->have_noise)
    {
        d->m->need_ft = 1;
        d->m->ldmap = d->m->Nside + 2;
    }
    else
    {
        d->m->need_ft = 0;
        d->m->ldmap = d->m->Nside;
    }

    SAFEALLOC(d->m->map_real, ((d->m->need_ft) ?
                               fftw_malloc : malloc)(d->m->Nside * d->m->ldmap
                                                     * sizeof(double)));

    if (d->m->need_ft)
    {
        d->m->map_comp = (double complex *)d->m->map_real;

        if (!(d->f->has_z_dependent))
        // if there are z-dependent filters, the 0th workspace
        //     handles the r2c FFTs (one for each redshift)
        {
            SAFEALLOC(d->m->p_r2c, malloc(sizeof(fftw_plan)));
            *(d->m->p_r2c) = fftw_plan_dft_r2c_2d(d->m->Nside, d->m->Nside,
                                                  d->m->map_real, d->m->map_comp,
                                                  FFTW_ESTIMATE);
        }

        SAFEALLOC(d->m->p_c2r, malloc(sizeof(fftw_plan)));
        *(d->m->p_c2r) = fftw_plan_dft_c2r_2d(d->m->Nside, d->m->Nside,
                                              d->m->map_comp, d->m->map_real,
                                              FFTW_ESTIMATE);
    }

    d->m->created_mem = 1;

    ENDFCT
}//}}}

static int
subtract_map_mean(hmpdf_obj *d)
{//{{{
    STARTFCT
    
    double mean = 0.0;
    for (long ii=0; ii<d->m->Nside; ii++)
    {
        for (long jj=0; jj<d->m->Nside; jj++)
        {
            mean += d->m->map_real[ii*d->m->ldmap+jj];
        }
    }
    
    mean /= (double)(d->m->Nside * d->m->Nside);

    for (long ii=0; ii<d->m->Nside; ii++)
    {
        for (long jj=0; jj<d->m->Nside; jj++)
        {
            d->m->map_real[ii*d->m->ldmap+jj] -= mean;
        }
    }

    ENDFCT
}//}}}

static int
create_map(hmpdf_obj *d)
{//{{{
    STARTFCT

    if (d->m->created_map) { return 0; }

    HMPDFPRINT(2, "\tcreate_map\n");

    // zero the map
    zero_real(d->m->Nside * d->m->ldmap, d->m->map_real);

    // run the loop
    if (d->f->has_z_dependent)
    {
        SAFEHMPDF(loop_w_z_dependence(d));
    }
    else
    {
        SAFEHMPDF(loop_no_z_dependence(d));
    }

    if (d->m->need_ft)
    {
        // add the Gaussian random field
        //     it is assumed that the noise power spectrum does NOT
        //     already contain the convolution with the other potential
        //     filters --> we do this first
        SAFEHMPDF(add_grf(d, d->ns->noise_pwr, d->ns->noise_pwr_params));

        // apply the filters (not z-dependent)
        SAFEHMPDF(filter_map(d, d->m->map_comp, NULL));

        // transform back to real space
        HMPDFCHECK(d->m->p_c2r == NULL,
                   "trying to execute an fftw_plan that has not been initialized.");
        fftw_execute(*(d->m->p_c2r));

        // normalize properly
        for (long ii=0; ii<d->m->Nside * (d->m->Nside+2); ii++)
        {
            d->m->map_real[ii] /= (double)(d->m->Nside * d->m->Nside);
        }
    }

    if (d->p->stype == hmpdf_kappa)
    {
        SAFEHMPDF(subtract_map_mean(d));
    }

    d->m->created_map = 1;

    ENDFCT
}//}}}

static int
create_sidelengths(hmpdf_obj *d)
{//{{{
    STARTFCT

    if (d->m->created_sidelengths) { return 0; }

    HMPDFPRINT(2, "\tcreate_sidelengths\n");

    double map_side = sqrt(d->m->area);
    d->m->Nside = (long)round(map_side/d->f->pixelside);

    double max_t_out = 0.0;
    for (int z_index=0; z_index<d->n->Nz; z_index++)
    {
        for (int M_index=0; M_index<d->n->NM; M_index++)
        {
            max_t_out = GSL_MAX(max_t_out,
                                d->p->profiles[z_index][M_index][0]);
        }
    }

    long temp = (long)round(max_t_out/d->f->pixelside);
    temp *= 2;
    temp += 4; // some safety buffer
    d->m->buflen = 2 * temp * temp; // not sufficient to do all halos
                                    // in one go, but long enough that it
                                    // is reasonably efficient

    d->m->created_sidelengths = 1;

    HMPDFPRINT(3, "\t\tmap = %ld x %ld <=> %g GB\n",
                  d->m->Nside, d->m->Nside,
                  1e-9 * (double)(d->m->Nside * d->m->Nside
                                  * sizeof(double)));
    HMPDFPRINT(3, "\t\tbuffer <=> %g GB\n",
                  1e-9 * (double)(d->m->buflen * sizeof(double)));

    ENDFCT
}//}}}

static int
prepare_maps(hmpdf_obj *d)
// compute map dimensions
// allocate workspaces
{//{{{
    STARTFCT

    HMPDFPRINT(1, "prepare_maps\n");

    SAFEHMPDF(create_sidelengths(d));
    SAFEHMPDF(create_mem(d));
    SAFEHMPDF(create_ellgrid(d));
    SAFEHMPDF(create_map_ws(d));
    SAFEHMPDF(create_map(d));

    ENDFCT
}//}}}

static int
common_input_processing(hmpdf_obj *d, int new_map)
{//{{{
    STARTFCT
    
    CHECKINIT;

    HMPDFCHECK(d->m->area < 0.0,
               "no/invalid sky fraction passed.");
    HMPDFCHECK(d->f->pixelside < 0.0,
               "no/invalid pixel sidelength passed.");

    if (!(d->m->created_map))
    {
        // if requested, initialize the system random number generator
        if (d->m->mapseed != INT_MAX)
        {
            srand((unsigned int)d->m->mapseed);
        }
        // otherwise initialize randomly
        else
        {
            srand((unsigned int)time(NULL));
        }
    }

    if (new_map)
    {
        d->m->created_map = 0;
    }

    SAFEHMPDF(prepare_maps(d));

    ENDFCT
}//}}}

int
hmpdf_get_map_op(hmpdf_obj *d, int Nbins, double binedges[Nbins+1], double op[Nbins], int new_map)
// if (new_map), create one
// else, if not available, create one
//       else, use the existing one
{//{{{
    STARTFCT

    HMPDFCHECK(not_monotonic(Nbins+1, binedges, 1),
               "binedges not monotonically increasing.");

    SAFEHMPDF(common_input_processing(d, new_map));
    
    // prepare a histogram
    gsl_histogram *h;
    SAFEALLOC(h, gsl_histogram_alloc(Nbins));
    SAFEGSL(gsl_histogram_set_ranges(h, binedges, Nbins+1));

    // if requested, use only part of the map
    long max_pix;
    if (d->m->usefrac > 0.0)
        max_pix = GSL_MIN(d->m->Nside, (long)round((double)(d->m->Nside) * sqrt(d->m->usefrac)));
    else
        max_pix = d->m->Nside;

    // accumulate the histogram
    for (long ii=0; ii<max_pix; ii++)
    {
        for (long jj=0; jj<max_pix; jj++)
        {
            double val = d->m->map_real[ii*d->m->ldmap+jj];
            if (val < binedges[0] || val >= binedges[Nbins])
            {
                continue;
            }
            else
            {
                SAFEGSL(gsl_histogram_increment(h, val));
            }
        }
    }

    // normalize
    SAFEGSL(gsl_histogram_scale(h, 1.0/(double)(max_pix * max_pix)));

    // write into output
    for (int ii=0; ii<Nbins; ii++)
    {
        op[ii] = gsl_histogram_get(h, ii);
    }

    gsl_histogram_free(h);

    ENDFCT
}//}}}

int
hmpdf_get_map_op_split(hmpdf_obj *d, int Nsplit, int Nbins, double binedges[Nbins+1],
                       double op[Nsplit*Nsplit][Nbins], int new_map)
// same as the above function, but splits the map into Nsplit * Nsplit sub-maps
// and measures the PDF in each
{//{{{
    STARTFCT

    HMPDFCHECK(not_monotonic(Nbins+1, binedges, 1),
               "binedges not monotonically increasing.");

    SAFEHMPDF(common_input_processing(d, new_map));
    
    // usefrac and this function are not compatible at the moment
    HMPDFCHECK(d->m->usefrac > 0.0, "hmpdf_map_usefrac not compatible with hmpdf_get_map_op_split");

    // the side length of each split map
    long split_Nside = d->m->Nside / Nsplit;

    // accumulate the histograms
    int nn = 0; // index counting the PDF
    for (int ii=0; ii<Nsplit; ii++)
    {
        for (int jj=0; jj<Nsplit; jj++)
        {
            // prepare a histogram
            gsl_histogram *h;
            SAFEALLOC(h, gsl_histogram_alloc(Nbins));
            SAFEGSL(gsl_histogram_set_ranges(h, binedges, Nbins+1));

            for (long kk=ii*split_Nside; kk<(ii+1)*split_Nside; kk++)
            {
                for (long ll=jj*split_Nside; ll<(jj+1)*split_Nside; ll++)
                {
                    double val = d->m->map_real[kk*d->m->ldmap+ll];
                    if (val < binedges[0] || val >= binedges[Nbins])
                    {
                        continue;
                    }
                    else
                    {
                        SAFEGSL(gsl_histogram_increment(h, val));
                    }
                }
            }

            // normalize
            SAFEGSL(gsl_histogram_scale(h, 1.0/(double)(split_Nside * split_Nside)));

            // write into output
            for (int kk=0; kk<Nbins; kk++)
            {
                op[nn][kk] = gsl_histogram_get(h, kk);
            }

            gsl_histogram_free(h);

            nn++;
        }
    }

    ENDFCT
}//}}}

int
perform_map_FT(hmpdf_obj *d)
// creates the fourier space representation of the map
//     in the 0th workspace (which is needed as buffer)
{//{{{
    STARTFCT

    // prepare the fftw plan if not already existing
    //     need to do this before copying data because creating
    //     an fftw plan does not preserve the memory pointed to
    if (d->m->ws[0]->p_r2c == NULL)
    {
        SAFEALLOC(d->m->ws[0]->p_r2c, malloc(sizeof(fftw_plan)));
        *(d->m->ws[0]->p_r2c) = fftw_plan_dft_r2c_2d(d->m->Nside, d->m->Nside,
                                                     d->m->ws[0]->map,
                                                     d->m->ws[0]->map_comp,
                                                     FFTW_ESTIMATE);
    }

    // copy the real space map into the 0th workspace
    for (long ii=0; ii<d->m->Nside; ii++)
    {
        memcpy(d->m->ws[0]->map + ii*d->m->ws[0]->ldmap,
               d->m->map_real + ii*d->m->ldmap,
               d->m->Nside * sizeof(double));
    }

    // create the fourier space map
    fftw_execute(*(d->m->ws[0]->p_r2c));

    ENDFCT
}//}}}

int
avg_bin_FT_map(hmpdf_obj *d, int Nbins, double binedges[Nbins+1], double ps[Nbins])
{//{{{
    STARTFCT

    // prepare two histograms
    //     (one to count number of modes, the other to accumulate mode powers
    gsl_histogram *h_nmodes;
    gsl_histogram *h_modepwrs;
    SAFEALLOC(h_nmodes, gsl_histogram_alloc(Nbins));
    SAFEALLOC(h_modepwrs, gsl_histogram_alloc(Nbins));
    SAFEGSL(gsl_histogram_set_ranges(h_nmodes, binedges, Nbins+1));
    SAFEGSL(gsl_histogram_set_ranges(h_modepwrs, binedges, Nbins+1));

    // loop over the Fourier space map
    for (long ii=0; ii<d->m->Nside; ii++)
    {
        double ell1 = WAVENR(d->m->Nside, d->m->ellgrid, ii);

        for (long jj=0; jj<d->m->Nside/2+1; jj++)
        {
            double ell2 = WAVENR(d->m->Nside, d->m->ellgrid, jj);
            double ellmod = hypot(ell1, ell2);

            if (ellmod < binedges[0] || ellmod > binedges[Nbins])
            {
                continue;
            }
            else
            {
                SAFEGSL(gsl_histogram_increment(h_nmodes, ellmod));
                SAFEGSL(gsl_histogram_accumulate(h_modepwrs, ellmod,
                                                 cabs(d->m->ws[0]->map_comp[ii*(d->m->Nside/2+1)+jj])));

            }
        }
    }

    // perform the averaging over modes
    SAFEGSL(gsl_histogram_div(h_modepwrs, h_nmodes));

    // write into output
    for (int ii=0; ii<Nbins; ii++)
    {
        ps[ii] = gsl_histogram_get(h_modepwrs, ii);
    }

    gsl_histogram_free(h_nmodes);
    gsl_histogram_free(h_modepwrs);

    ENDFCT
}//}}}

int
hmpdf_get_map_ps(hmpdf_obj *d, int Nbins, double binedges[Nbins+1], double ps[Nbins], int new_map)
{//{{{
    STARTFCT

    HMPDFCHECK(not_monotonic(Nbins+1, binedges, 1),
               "binedges not monotonically increasing.");

    SAFEHMPDF(common_input_processing(d, new_map));

    // create the Fourier space map
    SAFEHMPDF(perform_map_FT(d));

    // perform the binning
    SAFEHMPDF(avg_bin_FT_map(d, Nbins, binedges, ps));

    ENDFCT
}//}}}

int
_get_Nside(hmpdf_obj *d, long *Nside)
{//{{{
    STARTFCT
    CHECKINIT;
    SAFEHMPDF(create_sidelengths(d));
    *Nside = d->m->Nside;
    ENDFCT
}//}}}

int
hmpdf_get_map1(hmpdf_obj *d, double *map, int new_map)
{//{{{
    STARTFCT

    SAFEHMPDF(common_input_processing(d, new_map));

    for (long ii=0; ii<d->m->Nside; ii++)
    {
        memcpy(map + ii*d->m->Nside,
               d->m->map_real + ii*d->m->ldmap,
               d->m->Nside * sizeof(double));
    }

    ENDFCT
}//}}}

int
hmpdf_get_map(hmpdf_obj *d, double **map, long *Nside, int new_map)
{//{{{
    STARTFCT

    // we need to know how large to allocate
    SAFEHMPDF(create_sidelengths(d));

    if (map != NULL)
    {
        SAFEALLOC(*map, malloc(d->m->Nside * d->m->Nside * sizeof(double)));
        SAFEHMPDF(hmpdf_get_map1(d, *map, new_map));
    }

    if (Nside != NULL)
    {
        SAFEHMPDF(_get_Nside(d, Nside));
    }

    ENDFCT
}//}}}


