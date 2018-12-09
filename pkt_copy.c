/*
 * Copyright (c) 2016 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <x86intrin.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long long uword;
typedef double f64;
#define ARRAY_LEN(x) (sizeof (x) / sizeof (x[0]))

#include "memcpy_avx2.h"

/* typicall imix profile */
int imix_lengths[] = { 64, 1518, 64, 512, 64, 64, 512, 64, 64, 512, 64, 512 };

/* pin local and remote cpu to: */
int local_cpu = 1;
int remote_cpu = 2;

/* number of packets to copy */
int n_copy = 512;

/* arrays of src, dst and lengths */
void **dst_ptrs, **src_ptrs;
u32 *pkt_lengths;

/* dummy memory to flush data from L1 */
void *dummy;

volatile int remote_trigger = 0;

/* test function - to be optimized */
void
copy_packet_data (void **src, void **dst, u32 *lengths, u32 n_left)
{
  while (n_left >= 4)
    {
      _mm_prefetch (src[4], _MM_HINT_T0);
      _mm_prefetch (src[4] + 128, _MM_HINT_T0);
      _mm_prefetch (src[5], _MM_HINT_T0);
      _mm_prefetch (src[5] + 128, _MM_HINT_T0);

      clib_memcpy (dst[0], src[0], lengths[0]);
      clib_memcpy (dst[1], src[1], lengths[1]);

      _mm_prefetch (src[6], _MM_HINT_T0);
      _mm_prefetch (src[6] + 128, _MM_HINT_T0);
      _mm_prefetch (src[7], _MM_HINT_T0);
      _mm_prefetch (src[7] + 128, _MM_HINT_T0);

      clib_memcpy (dst[2], src[2], lengths[2]);
      clib_memcpy (dst[3], src[3], lengths[3]);

      /* next */
      dst += 4;
      src += 4;
      lengths += 4;
      n_left -= 4;
    }

  while (n_left)
    {
      clib_memcpy (dst[0], src[0], lengths[0]);

      /* next */
      dst += 1;
      src += 1;
      lengths += 1;
      n_left -= 1;
    }
}

void *
remote_core_fn (void *arg)
{
  cpu_set_t set;
  unsigned cpu, node;
  /* pin to cpu */
  CPU_ZERO (&set);
  CPU_SET (remote_cpu, &set);
  if (sched_setaffinity (0, sizeof (set), &set) == -1)
    {
      perror ("sched_setaffinity");
      exit (1);
    }
  syscall (__NR_getcpu, &cpu, &node, 0);
  printf ("remote thread running on cpu %u node %u\n", cpu, node);
  while (1)
    {
      while (remote_trigger == 0)
        _mm_pause ();
      /* touch every cacheline */
      for (int i = 0; i < n_copy; i++)
        for (int j = 0; j < pkt_lengths[i]; j += 64)
          *(u64 *)(src_ptrs[i] + j) += 1;
      _mm_sfence ();
      remote_trigger = 0;
    }
}

void *
alloc_aligned_and_lock (u32 size)
{
  void *rv;
  int err;

  rv = _mm_malloc (size, 64);
  if ((err = mlock (rv, size)))
    {
      printf ("mlock failed :[%s], try with sudo\n", strerror (err));
      exit (1);
    }
  return rv;
}

static inline f64
now (void)
{
  struct timespec ts;
  syscall (SYS_clock_gettime, CLOCK_REALTIME, &ts);
  return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

int
main (int argc, char *argv[])
{
  cpu_set_t set;
  int err;
  int n_cachelines = 0;
  u64 last_print = 0;
  unsigned cpu, node;
  pthread_t remote_core;
  f64 time_start, time_now;
  u64 total_bytes = 0, ticks_per_second;
  u64 t0, t1;

  if (argc > 1)
    n_copy = atoi (argv[1]);

  /* pin to cpu */
  CPU_ZERO (&set);
  CPU_SET (local_cpu, &set);
  if (sched_setaffinity (getpid (), sizeof (set), &set) == -1)
    {
      perror ("sched_setaffinity");
      exit (1);
    }

  /* measure TSC frequency */
  time_start = time_now = now ();
  t0 = _rdtsc ();
  while (time_now < time_start + 1)
    time_now = now ();
  t1 = _rdtsc ();

  ticks_per_second = round ((f64) (t1 - t0) * 1e-7) * 1e7;
  printf ("TSC frequency: %.02f GHz, tick duration %0.2f ns\n",
          (f64)ticks_per_second * 1e-9, 1e9 / ticks_per_second);

  dst_ptrs = alloc_aligned_and_lock (n_copy * sizeof (void *));
  src_ptrs = alloc_aligned_and_lock (n_copy * sizeof (void *));
  pkt_lengths = alloc_aligned_and_lock (n_copy * sizeof (u32));
  dummy = alloc_aligned_and_lock (4096 * 64);

  srand (time (0));

  for (int i = 0; i < n_copy; i++)
    {
      int l = imix_lengths[i % ARRAY_LEN (imix_lengths)];
      n_cachelines += ((l - 1) / 64) + 1;
      total_bytes += l;
      /* allocated some ramdom extra memoty at the end of each allocation */
      dst_ptrs[i] = alloc_aligned_and_lock (l + 64 * (rand () % 128));
      src_ptrs[i] = alloc_aligned_and_lock (l + 64 * (rand () % 128));
      pkt_lengths[i] = l;
    }

  /* start remote thread, remote thread runs on different cpu and updates
   * data so that thata stays in remote cache */
  if ((err = pthread_create (&remote_core, NULL, &remote_core_fn, NULL)))
    {
      printf ("\ncan't create thread :[%s]", strerror (err));
      exit (1);
    }

  syscall (__NR_getcpu, &cpu, &node, 0);
  printf ("main thread running on cpu %u node %u\n", cpu, node);

  while (1)
    {
      /* trigger remote thread */
      remote_trigger = 1;

      /* flush copy destination memory from L1 - randomly update dummy memory
       * so data from previous run is evicted from L1 */
      for (int i = 0; i < 4096; i++)
        *(u64 *)(dummy + 64 * (rand () % 4096)) += 1;

      /* wait for remote thread to complete */
      while (remote_trigger)
        _mm_pause ();

      t0 = _rdtsc ();
      copy_packet_data (dst_ptrs, src_ptrs, pkt_lengths, n_copy);
      t1 = _rdtsc ();

      /* print measuremrnt roughly every second */
      if (last_print + ticks_per_second < t0)
        {
          u64 ticks = t1 - t0;
          f64 bps = total_bytes * 8 * ticks_per_second / ticks;
          printf ("%llu ticks, %.2f ticks/packet, %.02f ticks/cacheline, "
                  "%.02f Gb/s\n",
                  ticks, (f64) (ticks) / n_copy, (f64) (ticks) / n_cachelines,
                  bps * 1e-9);
          last_print = t0;
        }
    }
}
