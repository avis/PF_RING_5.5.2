/*
 *
 * (C) 2005-12 - Luca Deri <deri@ntop.org>
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
 * MODIFICATIONS
 * 	This code is a mix of features from pwrite and pfcount. We have added
 * 	a more complete set of argument options to a code that is able to capture
 * 	packets and store them in pcap format. The additional arguments are: t for
 * 	specific interval for printing a line with capture statistics and c to set
 * 	an interval of capture.	The other arguments come from pfcount.
 *
 * 	This modifications were done by:
 * 		Ricardo de O. Schmidt
 * 		Idilio Drago
 *			Design and Analysis of Communication Systems (DACS)
 *			University of Twente (UT)
 *		December 11, 2012
 *
 */

#define _GNU_SOURCE

#include <pcap.h>
#include <signal.h>

#include "pfring.h"
#include "pfutils.c"

#define ALARM_SLEEP             3
#define DEFAULT_SNAPLEN       128
#define DEFAULT_DEVICE     "eth0"
#define NO_ZC_BUFFER_LEN     9000

pfring  *pd;
pcap_dumper_t *dumper = NULL;
pfring_stat pfringStats;
pthread_rwlock_t statsLock;

static struct timeval startTime;
unsigned long long numPkts = 0, numBytes = 0;
u_int8_t wait_for_packet = 1, do_shutdown = 0, add_drop_rule = 0;
u_int8_t use_extended_pkt_header = 0, enable_hw_timestamp = 0;
//long int curTs = 0; - rschmidt - not needed

/* rschmidt */
int alarm_sleep;
int capval;
long int capstop;

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

void print_stats() {
  pfring_stat pfringStat;
  static u_int64_t lastPkts = 0;
  static u_int64_t lastBytes = 0;
  static unsigned long long numLine = 0;
  static unsigned long long lastDrop = 0;

  if(pfring_stats(pd, &pfringStat) >= 0) {
    /* 1-line stats */
    printf("%llu sec pkts %llu drop %llu bytes %llu | pkts %llu bytes %llu drop %llu\n",
	   ++numLine,
	   numPkts-lastPkts,
	   pfringStat.drop-lastDrop,
	   numBytes-lastBytes,
	   numPkts, numBytes, (unsigned long long int)pfringStat.drop
	   );
    lastPkts = numPkts, lastBytes = numBytes, lastDrop = pfringStat.drop;
  }

}

/* ******************************** */

void sigproc(int sig) {
  static int called = 0;

  fprintf(stderr, "Leaving...\n");
  if(called) return; else called = 1;
  do_shutdown = 1;

  print_stats();

  pfring_breakloop(pd);
  //   pfring_close(pd);

}

/* ******************************** */

void my_sigalarm(int sig) {
  if(do_shutdown)
    return;

  print_stats();
  alarm(alarm_sleep);
  signal(SIGALRM, my_sigalarm);
}

/* ****************************************************** */

static char hex[] = "0123456789ABCDEF";

char* etheraddr_string(const u_char *ep, char *buf) {
  u_int i, j;
  char *cp;

  cp = buf;
  if((j = *ep >> 4) != 0)
    *cp++ = hex[j];
  else
    *cp++ = '0';

  *cp++ = hex[*ep++ & 0xf];

  for(i = 5; (int)--i >= 0;) {
    *cp++ = ':';
    if((j = *ep >> 4) != 0)
      *cp++ = hex[j];
    else
      *cp++ = '0';

    *cp++ = hex[*ep++ & 0xf];
  }

  *cp = '\0';
  return (buf);
}

/* *************************************** */

void printHelp(void) {
  printf("pfdump - (C) 2012 ntop.org and University of Twente\n\n");
  printf("-h              Print this help\n");
  printf("-i <device>     Device name. Use:\n"
	 "                - ethX@Y for channels\n"
	 "                - dnaX for DNA-based adapters\n"
	 "                - dnacluster:X for DNA cluster Id X\n"
#ifdef HAVE_DAG
	 "                - dag:dagX:Y for Endace DAG cards\n"
#endif
	 );
  printf("-f <filter>     [BPF filter]\n");
  printf("-e <direction>  0=RX+TX, 1=RX only, 2=TX only\n");
  printf("-s <len>        Packet capture length (snaplen)\n");
  printf("-w <dump file>  pcap dump file path\n");
  printf("-a              Active packet wait\n");
  printf("-t <time>       Periodic stats dump period (sec)\n");
  printf("-c <time>       Maximum capture duration (sec)\n");
}

/* *************************************** */

void* packet_consumer_thread(void* _id) {
  u_char buffer[NO_ZC_BUFFER_LEN];
  u_char *buffer_p = buffer;

  struct pfring_pkthdr hdr;

  memset(&hdr, 0, sizeof(hdr));

  while(1) {
    int rc;

    if(do_shutdown) break;

    if((rc = pfring_recv(pd, &buffer_p, NO_ZC_BUFFER_LEN, &hdr, wait_for_packet)) > 0) {
      if(do_shutdown) break;

      if (capval > 0) {
	if (capstop == 0) {
	  capstop = hdr.ts.tv_sec + capval;
	  printf("-> capstop set to %ld\n", capstop);
	}
	if (hdr.ts.tv_sec > capstop) {
	  break;
	}
      }

      pcap_dump((u_char*)dumper, (struct pcap_pkthdr*)&hdr, buffer), numPkts++;
      numBytes += hdr.len+24 /* 8 Preamble + 4 CRC + 12 IFG */;

      //			curTs = hdr.ts.tv_sec; /* current timestamp - rschmidt - not needed */
    } else {
      if(wait_for_packet == 0) sched_yield();
    }
  }

  return(NULL);
}

/* *************************************** */

int main(int argc, char* argv[]) {
  char *device = NULL, c, buf[32];
  u_char mac_address[6] = { 0 };
  int promisc, snaplen = DEFAULT_SNAPLEN, rc;
  u_int32_t flags = 0;
  packet_direction direction = rx_and_tx_direction;
  char *bpfFilter = NULL;

  startTime.tv_sec = 0;
  alarm_sleep = 1;
  capval=0; // by default leave 15 minutes

  while((c = getopt(argc,argv,"hi:ae:w:f:t:c:s:")) != '?') {
    if((c == 255) || (c == -1)) break;

    switch(c) {
    case 'h':
      printHelp();
      return(0);
      break;
    case 'a':
      wait_for_packet = 0;
      break;
    case 'e':
      switch(atoi(optarg)) {
      case rx_and_tx_direction:
      case rx_only_direction:
      case tx_only_direction:
	direction = atoi(optarg);
	break;
      }
      break;
    case 's':
      snaplen = atoi(optarg);
      break;
    case 'i':
      device = strdup(optarg);
      break;
    case 'f':
      bpfFilter = strdup(optarg);
      break;
    case 't':
      alarm_sleep = atoi(optarg);
      break;
    case 'c':
      capval = atoi(optarg);
      capstop = 0;
      break;
    case 'w':
      dumper = pcap_dump_open(pcap_open_dead(DLT_EN10MB, 16384 /* MTU */), optarg);
      if(dumper == NULL) {
        printf("Unable to open dump file %s\n", optarg);
        return(-1);
      }
      break;
    }
  }
  if(dumper == NULL) {
    printHelp();
    return(-1);
  }
  if(device == NULL) device = DEFAULT_DEVICE;

  /* hardcode: promisc=1, to_ms=500 */
  promisc = 1;

  pthread_rwlock_init(&statsLock, NULL);

  if (1)                      flags |= PF_RING_REENTRANT; // 2 threads
  if(promisc)                 flags |= PF_RING_PROMISC;
  flags |= PF_RING_DNA_SYMMETRIC_RSS;  /* Note that symmetric RSS is ignored by non-DNA drivers */

  pd = pfring_open(device, snaplen, flags);

  if(pd == NULL) {
    fprintf(stderr, "pfring_open error [%s] (pf_ring not loaded or perhaps you use quick mode and have already a socket bound to %s ?)\n",
	    strerror(errno), device);
    pcap_dump_close(dumper);
    return(-1);
  } else {
    u_int32_t version;

    pfring_set_application_name(pd, "pfdump");
    pfring_version(pd, &version);

    printf("Using PF_RING v.%d.%d.%d\n",
	   (version & 0xFFFF0000) >> 16,
	   (version & 0x0000FF00) >> 8,
	   version & 0x000000FF);
  }

  if(strstr(device, "dnacluster:")) {
    printf("Capturing from %s\n", device);
  } else {
    if(pfring_get_bound_device_address(pd, mac_address) != 0)
      fprintf(stderr, "Unable to read the device address\n");
    else {
      int ifindex = -1;

      pfring_get_bound_device_ifindex(pd, &ifindex);

      printf("Capturing from %s [%s][ifIndex: %d]\n",
	     device, etheraddr_string(mac_address, buf),
	     ifindex);
    }
  }

  printf("# Device RX channels: %d\n", pfring_get_num_rx_channels(pd));

  if(bpfFilter != NULL) {
    rc = pfring_set_bpf_filter(pd, bpfFilter);
    if(rc != 0)
      printf("pfring_set_bpf_filter(%s) returned %d\n", bpfFilter, rc);
    else
      printf("Successfully set BPF filter '%s'\n", bpfFilter);
  }

  if((rc = pfring_set_direction(pd, direction)) != 0)
    ; // fprintf(stderr, "pfring_set_direction returned %d (perhaps you use a direction other than rx only with DNA ?)\n", rc);

  if((rc = pfring_set_socket_mode(pd, recv_only_mode)) != 0)
    fprintf(stderr, "pfring_set_socket_mode returned [rc=%d]\n", rc);

  signal(SIGINT, sigproc);
  signal(SIGTERM, sigproc);
  signal(SIGINT, sigproc);
  signal(SIGALRM, my_sigalarm);
  alarm(alarm_sleep);

  if (pfring_enable_ring(pd) != 0) {
    printf("Unable to enable ring :-(\n");
    pfring_close(pd);
    pcap_dump_close(dumper);
    return(-1);
  }

  // Another thread consuming? Does it make any diff?
  pthread_t my_thread;
  pthread_create(&my_thread, NULL, packet_consumer_thread, (void*)0);
  pthread_join(my_thread, NULL);

  sleep(1);
  pfring_close(pd);
  pcap_dump_close(dumper);

  return(0);
}
