/*
This file is part of rtl-dab
trl-dab is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Foobar is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with rtl-dab.  If not, see <http://www.gnu.org/licenses/>.


david may 2012
david.may.muc@googlemail.com

*/

#include <assert.h>

#include "dab.h"
#include "dab_tables.h"
#include "input_sdr.h"
#include "sdr_sync.h"
#include "tunables.h"


void swap_blocks(int len, fftw_complex* sym) 
{
  fftw_complex tmp;
  int i;
  for (i = 0; i < len/2; i++) {
    tmp[0] = sym[i][0];
    tmp[1] = sym[i][1];
    sym[i][0] = sym[i + len/2][0];
    sym[i][1] = sym[i + len/2][1];
    sym[i + len/2][0] = tmp[0];
    sym[i + len/2][1] = tmp[1];
  }
}


void samples_fft_swap(fftw_complex* samples, fftw_complex* symbols)
{
  // fftw_plan fftw_plan_dft_1d(int n, fftw_complex *in, fftw_complex *out, int sign, unsigned flags); 
  fftw_plan p = fftw_plan_dft_1d(DAB_T_CS, samples, symbols, FFTW_FORWARD, FFTW_ESTIMATE);
  fftw_execute(p);
  fftw_destroy_plan(p);
  swap_blocks(DAB_T_CS, symbols);
}

// ret: a*conj(b) / normalizer
void dqpsk_step(fftw_complex a, fftw_complex b, fftw_complex *out, double normalizer) 
{
  // normalizer = b[0]*b[0] + b[1]*b[1];
  (*out)[0] = (a[0]*b[0] + a[1]*b[1]) / normalizer;
  (*out)[1] = (a[0]*b[1] - a[1]*b[0]) / normalizer;
}

int scalecount = 0;
int underflows = 0;
int overflows = 0;
int weaknums = 0;

// gain = 3.0:
// [12:112] DECODING STATS 0.033 0.035 0.26135     230400
// [12:113] DECODING STATS 0.175 0.180 0.10523     230400
// [12:114] DECODING STATS 0.278 0.281 0.07925     230400
// [12:115] DECODING STATS 0.185 0.189 0.10958     230400

// val: already normalized by dqpsk_step
uint8_t scale_double2ubyte(double val)
{
  /* int weak=128; */
  int mid = 128;
  int lo = mid - VITERBI_CLAMP_SCALE;
  int hi = mid + VITERBI_CLAMP_SCALE - 1;
  // fast variant for comparison
  /* return (val > 0) ? hi : lo; */

  scalecount++;
  double sval = val *  SCALING_GAIN;
  double nval = (VITERBI_SCALE-1)*sval + mid;

  // over/underflow on conversion is undefined!
  if (nval < lo) {
    /* underflows++; */
    return lo;
  }
  else if (nval > hi) {
    /* overflows++; */
    return hi;
  }
  else {
    /* if (weak/2 < nval && nval < 256-weak/2) { weaknums++; } */
    return nval;
  }
}

void print_decoding_stats() 
{
  fprintf(stderr, "decoding stats %4.3f %4.3f %7.5f %10d\n",
  	  1.0*underflows/scalecount, 1.0*overflows/scalecount, 1.0*weaknums/scalecount, scalecount);
  scalecount = 0;
  overflows = 0;
  underflows =0;
  weaknums=0;
}

double calcAvg(struct sdr_state_t *sdr) 
{
  double valueSum = 0;
  double valueSqSum = 0;
  int valueCount = 0;
  int valueSkip = 30; // not necessary to sample all values
  int j;
  for (j=0; j<DAB_SYMBOLS_IN_FRAME; j++) {  // j=0 is PRS, usable too!
    int empty_channels = DAB_T_CS - DAB_CARRIERS;
    // don't sum over unused channels, that just adds noise    
    int i;
    for (i = empty_channels/2; i < DAB_T_CS - empty_channels/2; i+=valueSkip) {
      valueCount++;
      double val = cpx_abs(sdr->symbols[j][i]);
      valueSum += val;
      /* valueSqSum += val*val; */
    }
  }
  valueSum /= valueCount;
  valueSqSum /= valueCount;
  valueSqSum -= valueSum*valueSum;
  /* fprintf(stderr, "value stats: avg %6.3f  stdddev: %6.3f\n", valueSum, sqrt(valueSqSum)); */
  return valueSum;
}

void dqpsk(struct sdr_state_t *sdr, double valueAvg)
{  
  int i, j;
  // j=1: skip/start with PRS
  double n = valueAvg*valueAvg;
  for (j=1; j<DAB_SYMBOLS_IN_FRAME; j++) {
    for (i=0; i<DAB_T_CS; i++) {
      // TODO handling unused carriers dosn't do any harm, but takes time
      dqpsk_step(sdr->symbols[j][i], sdr->symbols[j-1][i], &sdr->symbols_d[j*DAB_T_CS + i], n);
    }
  }
}

void deinterlaceScale(struct demapped_transmission_frame_t *tf, struct sdr_state_t *sdr)
{
  int i, j;
  uint8_t* dst = tf->fic_symbols_demapped[0];
  tf->has_fic = 1;  /* Always true for SDR input */

  int k,kk;
  for (j=1; j<DAB_SYMBOLS_IN_FRAME; j++) {
    if (j == 4) { dst = tf->msc_symbols_demapped[0]; }
    k = 0;
    for (i=0; i<DAB_T_CS; i++) {
      int empty_channels = DAB_T_CS - DAB_CARRIERS;
      // 256..1792 \ 1024
      if (empty_channels/2 <= i && i != DAB_T_CS/2 && i <= DAB_CARRIERS+empty_channels/2) {
        kk = rev_freq_deint_tab[k++];
        /* dst[kk]              = (sdr->symbols_d[j*DAB_T_CS + i][0] < 0); */
        /* dst[kk+DAB_CARRIERS] = (sdr->symbols_d[j*DAB_T_CS + i][1] > 0); */
        dst[kk]              = scale_double2ubyte(- sdr->symbols_d[j*DAB_T_CS + i][0]);
	dst[kk+DAB_CARRIERS] = scale_double2ubyte(sdr->symbols_d[j*DAB_T_CS + i][1]);
      }
    }
    dst += 2*DAB_CARRIERS;  // 3072  (2 bits per symbol*carrier)
  }
  /* print_decoding_stats(); */
}


// ret: status
// -99: unknown
// -4: queue not full enough, do again
// -3: gain settle time // TODO: do this in dab2etsi   // >= -3: full frame read
// -2: coarse time shift
// -1: coarse frequency shift // >=-1, reasonable time sync
//  0: read, only fine shifts
int sdr_demod(struct demapped_transmission_frame_t *tf, struct sdr_state_t *sdr) {
  int i,j;

  assert(DAB_T_FRAME == DAB_SYMBOLS_IN_FRAME * DAB_T_SYM + DAB_T_NULL);
  assert(DAB_T_SYM == DAB_T_CS + DAB_T_GUARD);

  tf->has_fic = 0;

  /* resetting coarse freqshift */
  sdr->coarse_freq_shift = 0;

  cbWriteN(&(sdr->fifo), sdr->input_buffer_len, sdr->input_buffer);

  // timeshifts in bytes; positive skip, negative unread from last time
  int offset = sdr->coarse_timeshift + sdr->fine_timeshift;

  /* Check for data in fifo */
  if ((int)sdr->fifo.count < 2*DAB_T_FRAME + offset) {
    // fprintf(stderr, "   no data in fifo %d < %d\n", sdr->fifo.count, DAB_T_FRAME * 3);
    return -4;
  }

  /* int ctsa = -99999; // dab_coarse_time_sync(sdr->real, 1)  // heavy debug only */
  /* fprintf(stderr, "offset: %6d; ctsa: %6d, cts: %6d fts: %6d \n", */
  /* 	  offset, ctsa, sdr->coarse_timeshift, sdr->fine_timeshift); */
  
  /* read fifo, 2 reads (Q/I) per sample */
  sdr_read_fifo(&(sdr->fifo), 2*DAB_T_FRAME, offset, sdr->buffer);
  sdr->fine_timeshift = 0; // time shift was applied as part of offset

  
  /* give the AGC some time to settle */
  if (sdr->startup_delay<=GAIN_SETTLE_TIME) {
    // (frame has been read and will be thrown away)
    sdr->startup_delay+=1;
    fprintf(stderr, "   startup_delay=%i\n",sdr->startup_delay);
    return -3;
  }


  /* complex data conversion (also unsigned byte to signed) ; aka I/Q-samples */
  for (j=0; j<DAB_T_FRAME*2; j+=2) {
    sdr->real[j/2] = sdr->buffer[j] - 127; // = SCHAR_MAX
    sdr->imag[j/2] = sdr->buffer[j+1] - 127;
  }
  /* create complex frame */
  for (j=0; j<DAB_T_FRAME; j++) {
    sdr->dab_frame[j][0] = sdr->real[j];
    sdr->dab_frame[j][1] = sdr->imag[j];
  }

  /* coarse time sync */
  /* performance bottleneck atm */
  sdr->coarse_timeshift = dab_coarse_time_sync(sdr->real, sdr->force_timesync);
  // we are not in sync so -> next frame
  sdr->force_timesync=0;
  if (sdr->coarse_timeshift != 0) {
    int newTime = time_sync_full(sdr->dab_frame);
    //fprintf(stderr, "   coarse time shift %d\n", sdr->coarse_timeshift);
    /* // FIXME: work in progress */
    sdr->coarse_timeshift = newTime;
    if (sdr->coarse_timeshift > 1000) {
      fprintf(stderr, "STATS: fifo: %4.2f ffs: ------  cfs: --; TIME: %4d %6d (%6d)\n", 
	      sdr->fifo.count/2.0/DAB_T_FRAME, sdr->fine_timeshift, sdr->coarse_timeshift, newTime);
      return -2;
    }
    else {
      sdr->fine_timeshift = sdr->coarse_timeshift;
      sdr->coarse_timeshift = 0;
    }
  }
  else {
    /* fine time sync */
    sdr->fine_timeshift = dab_fine_time_sync(sdr->dab_frame);
  }


  /* coarse_frequency shift */
  // removed fixme why 505=DAB_T_GUARD+1??
  for (j=0; j<CFS_SYM_COUNT; j++) {
    samples_fft_swap(&sdr->dab_frame[DAB_T_NULL + j*DAB_T_SYM + DAB_T_GUARD/2], sdr->symbols[j]);
  }
  int cfs4 = dab_coarse_freq_sync_4(CFS_SYM_COUNT, sdr->symbols);
  /* int cfs3 = dab_coarse_freq_sync_3(CFS_SYM_COUNT, sdr->symbols); */
  /* int cfs2 = dab_coarse_freq_sync_2(sdr->symbols[0]); */
  int coarse_freq_shift = cfs4;
  if (abs(coarse_freq_shift) >= 1) {
    sdr->coarse_freq_shift = coarse_freq_shift;
    sdr->force_timesync = 1;
    //fprintf(stderr, "   coarse freq shift, also force timeshift %d\n", sdr->coarse_freq_shift);
    // (even if req shift not applied immediately) return anyway to do the forced time sync
    if (abs(coarse_freq_shift) > 1) {
      fprintf(stderr, "STATS: fifo: %4.2f ffs: ------  CFS: %2d; time: %4d\n", 
	      sdr->fifo.count/2.0/DAB_T_FRAME, sdr->coarse_freq_shift, sdr->fine_timeshift);
      return -1;
    }
  }

  /* d-qpsk */
  // see https://en.wikipedia.org/wiki/Phase-shift_keying  (differential quad)
  // processing i=0..<CFS_SYM_COUNT (again) is needed if usage (phase) of DAB_T_GUARD above
  // is different from here, because that changes relative phase too much already!
  for (i=CFS_SYM_COUNT; i<DAB_SYMBOLS_IN_FRAME; i++) {  // i=0: PRS
    samples_fft_swap(&sdr->dab_frame[DAB_T_NULL + (DAB_T_SYM*i) + DAB_T_GUARD/2], sdr->symbols[i]);
  }

  double valueAvg = calcAvg(sdr);
  dqpsk(sdr, valueAvg);
  
  deinterlaceScale(tf, sdr);


  /* fine freq correction */
  sdr->fine_freq_shift = dab_fine_freq_corr(sdr->dab_frame);
  int newTime = 0;// time_sync_full(sdr->dab_frame);
  fprintf(stderr, "STATS: fifo: %4.2f ffs: %6.1f  cfs: %2d; time: %4d\n", 
	  sdr->fifo.count/2.0/DAB_T_FRAME, 
	  sdr->fine_freq_shift, sdr->coarse_freq_shift, 
	  sdr->fine_timeshift);

  //fprintf(stderr, "   ok\n");
  return 0;
}





void sdr_init(struct sdr_state_t *sdr)
{
  // circular buffer init
  cbInit(&(sdr->fifo), (DAB_T_FRAME*2*4)); // 4 frames; what is the 2? real/complex?
  // malloc of various buffers
  sdr->coarse_timeshift = 0;
  sdr->fine_timeshift=0;

  // malloc of various buffers
  sdr->symbols_d = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * DAB_T_CS * DAB_SYMBOLS_IN_FRAME);

  /* make sure to disable fault injection by default */
  sdr->p_e_prior_dep = 0.0f;
  sdr->p_e_prior_vitdec = 0.0f;
  sdr->p_e_after_vitdec = 0.0f;
}
