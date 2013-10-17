#include <zebra.h>
#include "miyagi.h"

#include "qlib_init.h"
#include "command.h"
#include "vty.h"
#include "stream.h"
#include "privs.h"
#include "memory.h"

#include "bgpd/bgp.h"
#include "bgpd/bgpd.h"
#include "bgpd/bgp_peer.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_debug.h"

#define VT100_RESET "\x1b[0m"
#define VT100_RED "\x1b[31m"
#define VT100_GREEN "\x1b[32m"
#define VT100_YELLOW "\x1b[33m"


#define CAPABILITY 0
#define DYNCAP     1
#define OPT_PARAM  2

static int failed = 0;
static int tty = 0;

/* test segments to parse and validate, and use for other tests */
static struct test_segment {
  const char *name;
  const char *desc;
  const u_char data[1024];
  int len;
#define SHOULD_PARSE	true
#define SHOULD_ERR	false
  int parses; /* whether it should parse or not */

  /* AFI/SAFI validation */
  afi_t afi;
  safi_t safi;
#define VALID_AFI 1
#define INVALID_AFI 0
  int afi_valid;
} mp_reach_segments [] =
{
  { "IPv6",
    "IPV6 MP Reach, global nexthop, 1 NLRI",
    {
      /* AFI / SAFI */		0x0, AFI_IP6, SAFI_UNICAST,
      /* nexthop bytes */	16,
      /* Nexthop (global) */	0xff, 0xfe, 0x1,  0x2,
                                0xaa, 0xbb, 0xcc, 0xdd,
                                0x3,  0x4,  0x5,  0x6,
                                0xa1, 0xa2, 0xa3, 0xa4,
      /* SNPA (defunct, MBZ) */	0x0,
      /* NLRI tuples */		32, 0xff, 0xfe, 0x1, 0x2, /* fffe:102::/32 */
    },
    (4 + 16 + 1 + 5),
    SHOULD_PARSE,
    AFI_IP6, SAFI_UNICAST, VALID_AFI,
  },
  { "IPv6-2",
    "IPV6 MP Reach, global nexthop, 2 NLRIs",
    {
      /* AFI / SAFI */		0x0, AFI_IP6, SAFI_UNICAST,
      /* nexthop bytes */	16,
      /* Nexthop (global) */	0xff, 0xfe, 0x1,  0x2,   /* ffee:102:... */
                                0xaa, 0xbb, 0xcc, 0xdd,
                                0x3,  0x4,  0x5,  0x6,
                                0xa1, 0xa2, 0xa3, 0xa4,
      /* SNPA (defunct, MBZ) */	0x0,
      /* NLRI tuples */		32,
                                0xff, 0xfe, 0x1, 0x2, /* fffe:102::/32 */
                                64,
                                0xff, 0xfe, 0x0, 0x1, /* fffe:1:2:3::/64 */
                                0x0,  0x2,  0x0, 0x3,
    },
    (4 + 16 + 1 + 5 + 9),
    SHOULD_PARSE,
    AFI_IP6, SAFI_UNICAST, VALID_AFI,
  },
  { "IPv6-default",
    "IPV6 MP Reach, global nexthop, 2 NLRIs + default",
    {
      /* AFI / SAFI */		0x0, AFI_IP6, SAFI_UNICAST,
      /* nexthop bytes */	16,
      /* Nexthop (global) */	0xff, 0xfe, 0x1,  0x2,
                                0xaa, 0xbb, 0xcc, 0xdd,
                                0x3,  0x4,  0x5,  0x6,
                                0xa1, 0xa2, 0xa3, 0xa4,
      /* SNPA (defunct, MBZ) */	0x0,
      /* NLRI tuples */		32,
                                0xff, 0xfe, 0x1, 0x2, /* fffe:102::/32 */
                                64,
                                0xff, 0xfe, 0x0, 0x1, /* fffe:1:2:3::/64 */
                                0x0,  0x2,  0x0, 0x3,
                                0x0, /* ::/0 */
    },
    (4 + 16 + 1 + 5 + 9 + 1),
    SHOULD_PARSE,
    AFI_IP6, SAFI_UNICAST, VALID_AFI,
  },
  { "IPv6-lnh",
    "IPV6 MP Reach, global+local nexthops, 2 NLRIs + default",
    {
      /* AFI / SAFI */		0x0, AFI_IP6, SAFI_UNICAST,
      /* nexthop bytes */	32,
      /* Nexthop (global) */	0xff, 0xfe, 0x1,  0x2,  /* fffe:102:... */
                                0xaa, 0xbb, 0xcc, 0xdd,
                                0x3,  0x4,  0x5,  0x6,
                                0xa1, 0xa2, 0xa3, 0xa4,
      /* Nexthop (local) */     0xfe, 0x80, 0x0,  0x0,  /* fe80::210:2ff:.. */
                                0x0,  0x0,  0x0,  0x0,
                                0x2,  0x10, 0x2,  0xff,
                                0x1,  0x2,  0x3,  0x4,
      /* SNPA (defunct, MBZ) */	0x0,
      /* NLRI tuples */		32,
                                0xff, 0xfe, 0x1, 0x2, /* fffe:102::/32 */
                                64,
                                0xff, 0xfe, 0x0, 0x1, /* fffe:1:2:3::/64 */
                                0x0,  0x2,  0x0, 0x3,
                                0x0, /* ::/0 */
    },
    (4 + 32 + 1 + 5 + 9 + 1),
    SHOULD_PARSE,
    AFI_IP6, SAFI_UNICAST, VALID_AFI,
  },
  { "IPv6-nhlen",
    "IPV6 MP Reach, inappropriate nexthop length",
    {
      /* AFI / SAFI */		0x0, AFI_IP6, SAFI_UNICAST,
      /* nexthop bytes */	4,
      /* Nexthop (global) */	0xff, 0xfe, 0x1,  0x2,  /* fffe:102:... */
                                0xaa, 0xbb, 0xcc, 0xdd,
                                0x3,  0x4,  0x5,  0x6,
                                0xa1, 0xa2, 0xa3, 0xa4,
      /* Nexthop (local) */     0xfe, 0x80, 0x0,  0x0,  /* fe80::210:2ff:.. */
                                0x0,  0x0,  0x0,  0x0,
                                0x2,  0x10, 0x2,  0xff,
                                0x1,  0x2,  0x3,  0x4,
      /* SNPA (defunct, MBZ) */	0x0,
      /* NLRI tuples */		32,
                                0xff, 0xfe, 0x1, 0x2, /* fffe:102::/32 */
                                64,
                                0xff, 0xfe, 0x0, 0x1, /* fffe:1:2:3::/64 */
                                0x0,  0x2,  0x0, 0x3,
                                0x0, /* ::/0 */
    },
    (4 + 32 + 1 + 5 + 9 + 1),
    SHOULD_ERR,
    AFI_IP6, SAFI_UNICAST, VALID_AFI,
  },
  { "IPv6-nhlen2",
    "IPV6 MP Reach, invalid nexthop length",
    {
      /* AFI / SAFI */		0x0, AFI_IP6, SAFI_UNICAST,
      /* nexthop bytes */	5,
      /* Nexthop (global) */	0xff, 0xfe, 0x1,  0x2,  /* fffe:102:... */
                                0xaa, 0xbb, 0xcc, 0xdd,
                                0x3,  0x4,  0x5,  0x6,
                                0xa1, 0xa2, 0xa3, 0xa4,
      /* Nexthop (local) */     0xfe, 0x80, 0x0,  0x0,  /* fe80::210:2ff:.. */
                                0x0,  0x0,  0x0,  0x0,
                                0x2,  0x10, 0x2,  0xff,
                                0x1,  0x2,  0x3,  0x4,
      /* SNPA (defunct, MBZ) */	0x0,
      /* NLRI tuples */		32,
                                0xff, 0xfe, 0x1, 0x2, /* fffe:102::/32 */
                                64,
                                0xff, 0xfe, 0x0, 0x1, /* fffe:1:2:3::/64 */
                                0x0,  0x2,  0x0, 0x3,
                                0x0, /* ::/0 */
    },
    (4 + 32 + 1 + 5 + 9 + 1),
    SHOULD_ERR,
    AFI_IP6, SAFI_UNICAST, VALID_AFI,
  },
  { "IPv6-nhlen3",
    "IPV6 MP Reach, nexthop length overflow",
    {
      /* AFI / SAFI */		0x0, AFI_IP6, SAFI_UNICAST,
      /* nexthop bytes */	32,
      /* Nexthop (global) */	0xff, 0xfe, 0x1,  0x2,  /* fffe:102:... */
                                0xaa, 0xbb, 0xcc, 0xdd,
                                0x3,  0x4,  0x5,  0x6,
                                0xa1, 0xa2, 0xa3, 0xa4,
    },
    (4 + 16),
    SHOULD_ERR,
    AFI_IP6, SAFI_UNICAST, VALID_AFI,
  },
  { "IPv6-nhlen4",
    "IPV6 MP Reach, nexthop length short",
    {
      /* AFI / SAFI */		0x0, AFI_IP6, SAFI_UNICAST,
      /* nexthop bytes */	16,
      /* Nexthop (global) */	0xff, 0xfe, 0x1,  0x2,  /* fffe:102:... */
                                0xaa, 0xbb, 0xcc, 0xdd,
                                0x3,  0x4,  0x5,  0x6,
                                0xa1, 0xa2, 0xa3, 0xa4,
      /* Nexthop (local) */     0xfe, 0x80, 0x0,  0x0,  /* fe80::210:2ff:.. */
                                0x0,  0x0,  0x0,  0x0,
                                0x2,  0x10, 0x2,  0xff,
                                0x1,  0x2,  0x3,  0x4,
      /* SNPA (defunct, MBZ) */	0x0,
      /* NLRI tuples */		32,
                                0xff, 0xfe, 0x1, 0x2, /* fffe:102::/32 */
                                64,
                                0xff, 0xfe, 0x0, 0x1, /* fffe:1:2:3::/64 */
                                0x0,  0x2,  0x0, 0x3,
                                0x0, /* ::/0 */
    },
    (4 + 32 + 1 + 5 + 9 + 1),
    SHOULD_ERR,
    AFI_IP6, SAFI_UNICAST, VALID_AFI,
  },
  { "IPv6-nlri",
    "IPV6 MP Reach, NLRI bitlen overflow",
    {
      /* AFI / SAFI */		0x0, AFI_IP6, SAFI_UNICAST,
      /* nexthop bytes */	32,
      /* Nexthop (global) */	0xff, 0xfe, 0x1,  0x2,  /* fffe:102:... */
                                0xaa, 0xbb, 0xcc, 0xdd,
                                0x3,  0x4,  0x5,  0x6,
                                0xa1, 0xa2, 0xa3, 0xa4,
      /* Nexthop (local) */     0xfe, 0x80, 0x0,  0x0,  /* fe80::210:2ff:.. */
                                0x0,  0x0,  0x0,  0x0,
                                0x2,  0x10, 0x2,  0xff,
                                0x1,  0x2,  0x3,  0x4,
      /* SNPA (defunct, MBZ) */	0x0,
      /* NLRI tuples */		120,
                                0xff, 0xfe, 0x1, 0x2, /* fffe:102::/32 */
                                64,
                                0xff, 0xfe, 0x0, 0x1, /* fffe:1:2:3::/64 */
                                0x0,  0x2,  0x0, 0x3,
                                0, /* ::/0 */
    },
    (4 + 32 + 1 + 5 + 9 + 1),
    SHOULD_ERR,
    AFI_IP6, SAFI_UNICAST, VALID_AFI,
  },
  { "IPv4",
    "IPv4 MP Reach, 2 NLRIs + default",
    {
      /* AFI / SAFI */		0x0, AFI_IP, SAFI_UNICAST,
      /* nexthop bytes */	4,
      /* Nexthop */		192, 168,   0,  1,
      /* SNPA (defunct, MBZ) */	0x0,
      /* NLRI tuples */		16, 10, 1,    /* 10.1/16 */
                                17, 10, 2, 3, /* 10.2.3/17 */
                                0, /* 0/0 */
    },
    (4 + 4 + 1 + 3 + 4 + 1),
    SHOULD_PARSE,
    AFI_IP, SAFI_UNICAST, VALID_AFI,
  },
  { "IPv4-nhlen",
    "IPv4 MP Reach, nexthop lenth overflow",
    {
      /* AFI / SAFI */		0x0, AFI_IP, SAFI_UNICAST,
      /* nexthop bytes */	32,
      /* Nexthop */		192, 168,   0,  1,
      /* SNPA (defunct, MBZ) */	0x0,
      /* NLRI tuples */		16, 10, 1,    /* 10.1/16 */
                                17, 10, 2, 3, /* 10.2.3/17 */
                                0, /* 0/0 */
    },
    (4 + 4 + 1 + 3 + 4 + 1),
    SHOULD_ERR,
    AFI_IP, SAFI_UNICAST, VALID_AFI,
  },
  { "IPv4-nlrilen",
    "IPv4 MP Reach, nlri lenth overflow",
    {
      /* AFI / SAFI */		0x0, AFI_IP, SAFI_UNICAST,
      /* nexthop bytes */	4,
      /* Nexthop */		192, 168,   0,  1,
      /* SNPA (defunct, MBZ) */	0x0,
      /* NLRI tuples */		16, 10, 1,    /* 10.1/16 */
                                30, 10,
                                0, /* 0/0 */
    },
    (4 + 4 + 1 + 3 + 2 + 1),
    SHOULD_ERR,
    AFI_IP, SAFI_UNICAST, VALID_AFI,
  },
  { "IPv4-MLVPN",
    "IPv4/MPLS-labeled VPN MP Reach, RD, Nexthop, 3 NLRIs",
    {
      /* AFI / SAFI */		0x0, AFI_IP, SAFI_MPLS_VPN,
      /* nexthop bytes */	12,
      /* RD */			0, 0, 1, 2,
                                0, 0xff, 3, 4,
      /* Nexthop */		192, 168,   0,  1,
      /* SNPA (defunct, MBZ) */	0x0,
      /* NLRI tuples */		16, 10, 1,    /* 10.1/16 */
                                17, 10, 2, 3,  /* 10.2.3/17 */
                                0, /* 0/0 */
    },
    (4 + 12 + 1 + 3 + 4 + 1),
    SHOULD_PARSE,
    AFI_IP, SAFI_UNICAST, VALID_AFI,
  },
  /* From bug #385 */
  { "IPv6-bug",
    "IPv6, global nexthop, 1 default NLRI",
    {
      /* AFI / SAFI */		0x0, 0x2, 0x1,
      /* nexthop bytes */	0x20,
      /* Nexthop (global) */	0x20, 0x01, 0x04, 0x70,
                                0x00, 0x01, 0x00, 0x06,
                                0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x01,
      /* Nexthop (local) */     0xfe, 0x80, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00,
                                0x02, 0x0c, 0xdb, 0xff,
                                0xfe, 0xfe, 0xeb, 0x00,
      /* SNPA (defunct, MBZ) */	0,
      /* NLRI tuples */		/* Should have 0 here for ::/0, but dont */
    },
    37,
    SHOULD_ERR,
    AFI_IP6, SAFI_UNICAST, VALID_AFI,
  },

  { NULL, NULL, {0}, 0, 0}
};

/* MP_UNREACH_NLRI tests */
static struct test_segment mp_unreach_segments [] =
{
  { "IPv6-unreach",
    "IPV6 MP Unreach, 1 NLRI",
    {
      /* AFI / SAFI */		0x0, AFI_IP6, SAFI_UNICAST,
      /* NLRI tuples */		32, 0xff, 0xfe, 0x1, 0x2, /* fffe:102::/32 */
    },
    (3 + 5),
    SHOULD_PARSE,
    AFI_IP6, SAFI_UNICAST, VALID_AFI,
  },
  { "IPv6-unreach2",
    "IPV6 MP Unreach, 2 NLRIs",
    {
      /* AFI / SAFI */		0x0, AFI_IP6, SAFI_UNICAST,
      /* NLRI tuples */		32,
                                0xff, 0xfe, 0x1, 0x2, /* fffe:102::/32 */
                                64,
                                0xff, 0xfe, 0x0, 0x1, /* fffe:1:2:3::/64 */
                                0x0,  0x2,  0x0, 0x3,
    },
    (3 + 5 + 9),
    SHOULD_PARSE,
    AFI_IP6, SAFI_UNICAST, VALID_AFI,
  },
  { "IPv6-unreach-default",
    "IPV6 MP Unreach, 2 NLRIs + default",
    {
      /* AFI / SAFI */		0x0, AFI_IP6, SAFI_UNICAST,
      /* NLRI tuples */		32,
                                0xff, 0xfe, 0x1, 0x2, /* fffe:102::/32 */
                                64,
                                0xff, 0xfe, 0x0, 0x1, /* fffe:1:2:3::/64 */
                                0x0,  0x2,  0x0, 0x3,
                                0x0, /* ::/0 */
    },
    (3 + 5 + 9 + 1),
    SHOULD_PARSE,
    AFI_IP6, SAFI_UNICAST, VALID_AFI,
  },
  { "IPv6-unreach-nlri",
    "IPV6 MP Unreach, NLRI bitlen overflow",
    {
      /* AFI / SAFI */		0x0, AFI_IP6, SAFI_UNICAST,
      /* NLRI tuples */		120,
                                0xff, 0xfe, 0x1, 0x2, /* fffe:102::/32 */
                                64,
                                0xff, 0xfe, 0x0, 0x1, /* fffe:1:2:3::/64 */
                                0x0,  0x2,  0x0, 0x3,
                                0, /* ::/0 */
    },
    (3 + 5 + 9 + 1),
    SHOULD_ERR,
    AFI_IP6, SAFI_UNICAST, VALID_AFI,
  },
  { "IPv4-unreach",
    "IPv4 MP Unreach, 2 NLRIs + default",
    {
      /* AFI / SAFI */		0x0, AFI_IP, SAFI_UNICAST,
      /* NLRI tuples */		16, 10, 1,    /* 10.1/16 */
                                17, 10, 2, 3, /* 10.2.3/17 */
                                0, /* 0/0 */
    },
    (3 + 3 + 4 + 1),
    SHOULD_PARSE,
    AFI_IP, SAFI_UNICAST, VALID_AFI,
  },
  { "IPv4-unreach-nlrilen",
    "IPv4 MP Unreach, nlri length overflow",
    {
      /* AFI / SAFI */		0x0, AFI_IP, SAFI_UNICAST,
      /* NLRI tuples */		16, 10, 1,    /* 10.1/16 */
                                30, 10,
                                0, /* 0/0 */
    },
    (3 + 3 + 2 + 1),
    SHOULD_ERR,
    AFI_IP, SAFI_UNICAST, VALID_AFI,
  },
  { "IPv4-unreach-MLVPN",
    "IPv4/MPLS-labeled VPN MP Unreach, RD, 3 NLRIs",
    {
      /* AFI / SAFI */		0x0, AFI_IP, SAFI_MPLS_VPN,
      /* nexthop bytes */	12,
      /* RD */			0, 0, 1, 2,
                                0, 0xff, 3, 4,
      /* Nexthop */		192, 168,   0,  1,
      /* SNPA (defunct, MBZ) */	0x0,
      /* NLRI tuples */		16, 10, 1,    /* 10.1/16 */
                                17, 10, 2, 3,  /* 10.2.3/17 */
                                0, /* 0/0 */
    },
    (3 + 3 + 4 + 1),
    SHOULD_PARSE,
    AFI_IP, SAFI_UNICAST, VALID_AFI,
  },
  { NULL, NULL, {0}, 0, 0}
};


/* basic parsing test */
static void
parse_test (bgp_peer peer, struct test_segment *t, int type)
{
  stream ibuf ;
  byte flags ;
  bool parsed ;
  int oldfailed = failed;
  bgp_attr_parsing_t args[1] ;
  const byte* attr_p ;

#define RANDOM_FUZZ 35

  memset (args, 0, sizeof (args));

  ibuf = stream_new(4096) ;

  stream_reset (ibuf);
  stream_put (ibuf, NULL, RANDOM_FUZZ);
  stream_set_getp (ibuf, RANDOM_FUZZ);

  flags = BGP_ATF_OPTIONAL | ((t->len < 256) ? 0 : BGP_ATF_EXTENDED) ;

  args->start_p = stream_get_pnt(ibuf) ;

  stream_putc(ibuf, flags) ;
  stream_putc(ibuf, type) ;
  if (flags & BGP_ATF_EXTENDED)
    stream_putw(ibuf, t->len) ;
  else
    stream_putc(ibuf, t->len) ;

  attr_p = stream_get_end(ibuf) ;

  stream_put (ibuf, t->data, t->len);

  args->end_p = stream_get_end(ibuf) ;

  printf ("%s: %s\n", t->name, t->desc);

#if 0
  args->peer   = peer ;
#endif

  args->type    = type ;
  args->flags   = flags ;
  args->length  = t->len ;

  if (type == BGP_ATT_MP_REACH_NLRI)
    attr_p = tx_bgp_attr_mp_reach_parse (args, attr_p);
  else
    attr_p = tx_bgp_attr_mp_unreach_parse (args, attr_p,
                                     stream_get_data(ibuf), args->end_p);

  parsed = (args->ret == BGP_ATTR_PARSE_OK) ;

  if (!parsed)
    {
#if 0
      safi_t safi = t->safi;

      if (bgp_afi_safi_valid_indices (t->afi, safi) != t->afi_valid)
        failed++;

      printf ("MP: %u/%u (%u): recv %u, nego %u\n",
              t->afi, t->safi, safi,
              peer->af_rcv[qafx_from_i(t->afi, safi)],
              peer->af_use[qafx_from_i(t->afi, safi)]);
#endif
    } ;

  if (t->parses != parsed)
    failed++ ;

  printf ("parsed?: %s\n", parsed ? "no" : "yes");

  if (tty)
    printf ("%s", (failed > oldfailed) ? VT100_RED "failed!" VT100_RESET
                                       : VT100_GREEN "OK" VT100_RESET);
  else
    printf ("%s", (failed > oldfailed) ? "failed!" : "OK" );

  if (failed)
    printf (" (%u)", failed);

  printf ("\n\n");
  stream_free(ibuf) ;
}

static bgp_inst bgp;
static as_t asn = 100;

static bgp_peer peer_create_accept (bgp_inst bgp) ;

int
main (int argc, char **argv)
{
  bgp_peer peer;
  int i, j;

  conf_bgp_debug_fsm = -1U;
  conf_bgp_debug_events = -1U;
  conf_bgp_debug_packet = -1U;
  conf_bgp_debug_normal = -1U;
  conf_bgp_debug_as4 = -1U;
  term_bgp_debug_fsm = -1U;
  term_bgp_debug_events = -1U;
  term_bgp_debug_packet = -1U;
  term_bgp_debug_normal = -1U;
  term_bgp_debug_as4 = -1U;

  qlib_init_first_stage(0);     /* Absolutely first     */
  host_init(argv[0]) ;

  bgp_master_init ();

  if (fileno (stdout) >= 0)
    tty = isatty (fileno (stdout));

  bgp = NULL ;

#if 0
  if (bgp_get (&bgp, &asn, NULL))
    return -1;
#endif

  peer = peer_create_accept (bgp);

  if (peer == NULL)
    return failed ;
#if 0
  peer->host = miyagi("foo") ;
#endif

  for (i = AFI_IP; i < AFI_MAX; i++)
    for (j = SAFI_UNICAST; j < SAFI_MAX; j++)
      {
#if 0
        peer->afc[qafx_from_i(i ,j)] = 1;
        peer->af_adv[qafx_from_i(i ,j)] = 1;
#endif
      }

  i = 0;
  while (mp_reach_segments[i].name)
    parse_test (peer, &mp_reach_segments[i++], BGP_ATT_MP_REACH_NLRI);

  i = 0;
  while (mp_unreach_segments[i].name)
    parse_test (peer, &mp_unreach_segments[i++], BGP_ATT_MP_UNREACH_NLRI);

  printf ("failures: %d\n", failed);
  return failed;
}

/*------------------------------------------------------------------------------
 * Make accept BGP peer... used in test programs...  TODO
 */
bgp_peer peer_create_accept (bgp_inst bgp)
{
#if 0
  bgp_peer peer;

  peer = bgp_peer_new (bgp, PEER_TYPE_REAL);

  peer = bgp_peer_lock (peer); /* bgp peer list reference */
  listnode_add_sort (bgp->peer, peer);

  return peer;
#endif
  return NULL ;
}
