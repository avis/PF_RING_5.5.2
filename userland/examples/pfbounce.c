/*
 *
 * (C) 2005-10 - Luca Deri <deri@ntop.org>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * VLAN support courtesy of Vincent Magnin <vincent.magnin@ci.unil.ch>
 *
 */

#define _GNU_SOURCE
#include <signal.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/poll.h>
#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <net/ethernet.h>     /* the L2 protocols */
#include <sys/time.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "pfring.h"
#include "pfutils.c"

pfring  *pd;
pfring_stat pfringStats;
char *in_dev = NULL;
u_int8_t wait_for_packet = 1, do_shutdown = 0;

#define DEFAULT_DEVICE     "eth0"

/* *************************************** */
/*
 * The time difference in millisecond
 */
double delta_time (struct timeval * now,
		   struct timeval * before) {
  time_t delta_seconds;
  time_t delta_microseconds;

  /*
   * compute delta in second, 1/10's and 1/1000's second units
   */
  delta_seconds      = now -> tv_sec  - before -> tv_sec;
  delta_microseconds = now -> tv_usec - before -> tv_usec;

  if(delta_microseconds < 0) {
    /* manually carry a one from the seconds field */
    delta_microseconds += 1000000;  /* 1e6 */
    -- delta_seconds;
  }
  return((double)(delta_seconds * 1000) + (double)delta_microseconds/1000);
}

/* ******************************** */

/* ******************************** */

void sigproc(int sig) {
  static int called = 0;

  fprintf(stderr, "Leaving...\n");
  if(called) return; else called = 1;
  do_shutdown = 1;

  pfring_close(pd);

  exit(0);
}

/* *************************************** */

void printHelp(void) {
  printf("pfbounce\n(C) 2010 Deri Luca <deri@ntop.org>\n\n");

  printf("pfbounce -i in_dev\n");
  printf("-h              Print this help\n");
  printf("-i <device>     Device name. Use device\n");
  printf("-v              Verbose\n");
  exit(0);
}

/* *************************************** */

void bouncer() {
  filtering_rule rule;

  /* Bounce all packets received on in_dev -> in_dev */
  memset(&rule, 0, sizeof(rule));
  rule.rule_id = 1;
  rule.rule_action = bounce_packet_and_stop_rule_evaluation;
  rule.core_fields.proto = 0; /* any */
  snprintf(rule.reflector_device_name, REFLECTOR_NAME_LEN, "%s", in_dev);

  if(pfring_add_filtering_rule(pd, &rule) < 0) {
    printf("pfring_add_filtering_rule() failed\n");
    pfring_close(pd);
    exit(-1);
  } else
    printf("Bounceing packets received on %s to %s\n", in_dev, in_dev);

  while(1) sleep(60);
}

/* *************************************** */

int main(int argc, char* argv[]) {
  char c;

  while((c = getopt(argc,argv,"hi:")) != -1) {
    switch(c) {
    case 'h':
      printHelp();      
      break;
    case 'i':
      in_dev = strdup(optarg);
      break;
    }
  }

  if(in_dev == NULL)  printHelp();

  printf("Capturing from %s\n", in_dev);

  pd = pfring_open(in_dev, 1500 /* snaplen */, PF_RING_PROMISC);
  if(pd == NULL) {
    printf("pfring_open %s error [%s]\n", in_dev, strerror(errno));
    return(-1);
  } else {
    u_int32_t version;

    pfring_set_application_name(pd, "pfbounce");
    pfring_version(pd, &version);

    printf("Using PF_RING v.%d.%d.%d\n", (version & 0xFFFF0000) >> 16, 
	   (version & 0x0000FF00) >> 8, version & 0x000000FF);
  }

  /* Bounce only RX packets */
  pfring_set_direction(pd, rx_only_direction);

  signal(SIGINT, sigproc);
  signal(SIGTERM, sigproc);
  signal(SIGINT, sigproc);

  pfring_enable_ring(pd);
  bouncer();

  pfring_close(pd);

  sleep(3);

  return(0);
}
