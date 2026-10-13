/*
 * Motorcomm YT9215 / YT9218 unmanaged Ethernet switch driver for U-Boot.
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#ifndef _IPQ_YT9215_H
#define _IPQ_YT9215_H

#include <common.h>
#include <net.h>

/*
 * ==========================================================================
 * YT9215 DTS device tree binding
 * ==========================================================================
 *
 * The IPQ5018 board file walks every /gmac_cfg child node as a GMAC
 * description, so the switch properties are flat members of the owning
 * GMAC node instead of a nested switch@N child:
 *
 *   gmac_cfg {
 *       gmac2_cfg {
 *           unit = <1>;
 *           base = <0x39D00000>;
 *           phy_address = <0x1d>;
 *
 *           yt9215_switch_enable = <1>;    // 1 = bring up the YT9215
 *           yt9215_mdio_addr     = <0x1d>; // Clause-22 SMI address
 *           yt9215_rst_gpio      = <39>;   // HW reset GPIO, 0 = skip reset
 *           yt9215_cpu_port      = <4>;    // CPU port MAC id   (default 4)
 *           yt9215_serdes_port   = <8>;    // SerDes uplink MAC (default 8)
 *           yt9215_serdes_speed  = <2500>; // 1000 or 2500     (default 2500)
 *       };
 *   };
 *
 * Property reference
 * -------------------------------------------------------------------------
 * Property              | Type | Required | Default | Description
 * -------------------------------------------------------------------------
 * yt9215_switch_enable  | u32  | Yes      | 0       | 1 = enable the switch
 * yt9215_mdio_addr      | u32  | Yes      | 0x1d    | SMI/MDIO address
 * yt9215_rst_gpio       | u32  | No       | 0       | HW reset GPIO, 0 = none
 * yt9215_cpu_port       | u32  | No       | 4       | CPU port MAC id
 * yt9215_serdes_port    | u32  | No       | 8       | SerDes uplink MAC id
 * yt9215_serdes_speed   | u32  | No       | 2500    | SerDes speed, 1000/2500
 * -------------------------------------------------------------------------
 *
 * Port layout (MAC ids) of a YT9215S wired the way the Xiaomi CR8818 does:
 *
 *   MAC0-3 : four internal GE PHYs, the user-facing LAN ports
 *   MAC4   : CPU/RGMII uplink, wired to the SoC's internal GE PHY
 *   MAC8   : SerDes uplink (SGMII/2500BASE-X) to the SoC UNIPHY
 *
 * The driver keeps every port in VLAN 1, untagged, so the switch behaves as a
 * plain Layer-2 fabric and the CPU ports see the user frames without any
 * switch tag.
 * ==========================================================================
 */

/* Clause-22 SMI "slots": (switchid << 2) | (slot << 1) | rw, switchid = 0 */
#define YT9215_SMI_ADDR_WRITE		0
#define YT9215_SMI_ADDR_READ		1
#define YT9215_SMI_DATA_WRITE		2
#define YT9215_SMI_DATA_READ		3

/* Switch global registers (accessed through the Clause-22 SMI protocol) */
#define YT9215_CHIP_ID_REG		0x80008
#define YT9215_GLOBAL_CTRL1_REG		0x80004
#define   YT9215_GLOBAL_CTRL1_MIB_EN	BIT(1)
#define YT9215_INTERFACE_CTRL_REG	0x80028
#define   YT9215_INTERFACE_CTRL_EXTIF0_EN	BIT(0)
#define   YT9215_INTERFACE_CTRL_EXTIF1_EN	BIT(1)
#define YT9215_CHIP_INTERFACE_SEL_REG	0x80394
#define   YT9215_EXTIF_SEL_EXTIF0_MASK		BIT(1)
#define   YT9215_EXTIF_SEL_EXTIF0_SERDES	0
#define   YT9215_EXTIF_SEL_EXTIF0_XMII		BIT(1)
#define   YT9215_EXTIF_SEL_EXTIF1_MASK		BIT(0)
#define   YT9215_EXTIF_SEL_EXTIF1_SERDES	0
#define   YT9215_EXTIF_SEL_EXTIF1_XMII		BIT(0)

/* SGMII_CTRLm: base 0x8008c, stride 4, one per SerDes instance (port 8 -> 0) */
#define YT9215_SGMII_CTRL_REG(macid)	(0x8008c + ((macid) * 0x4))
#define   YT9215_SGMII_CTRL_MODE_OFFSET		7
#define   YT9215_SGMII_CTRL_MODE_MASK		(0x7U << 7)
#define   YT9215_SGMII_CTRL_MODE_SGMII_PHY	1
#define   YT9215_SGMII_CTRL_MODE_2500BASEX	4
#define   YT9215_SGMII_CTRL_LINK		BIT(4)
#define   YT9215_SGMII_CTRL_DUPLEX_FULL		BIT(3)
#define   YT9215_SGMII_CTRL_SPEED_MASK		(0x7U << 0)
#define   YT9215_SGMII_CTRL_SPEED_1000		2
#define   YT9215_SGMII_CTRL_SPEED_2500		4

/* Chip id values (upper 16 bits of CHIP_ID_REG) */
#define YT9215_CHIP_ID_9215		0x9002
#define YT9215_CHIP_ID_9218		0x9001

/* PORT_CTRLm: base 0x80100, stride 4, one per MAC */
#define YT9215_PORT_CTRL_REG(macid)	(0x80100 + ((macid) * 0x4))
#define   YT9215_PORT_CTRL_FLOW_LINK_AN		BIT(10)
#define   YT9215_PORT_CTRL_AN_LINK_EN		BIT(9)
#define   YT9215_PORT_CTRL_DUPLEX_MODE		BIT(7)
#define   YT9215_PORT_CTRL_RX_FC_EN		BIT(6)
#define   YT9215_PORT_CTRL_TX_FC_EN		BIT(5)
#define   YT9215_PORT_CTRL_RXMAC_EN		BIT(4)
#define   YT9215_PORT_CTRL_TXMAC_EN		BIT(3)
#define   YT9215_PORT_CTRL_SPEED_MODE_MASK	(0x7U << 0)
#define   YT9215_PORT_CTRL_SPEED_10M		0
#define   YT9215_PORT_CTRL_SPEED_100M		1
#define   YT9215_PORT_CTRL_SPEED_1000M		2
#define   YT9215_PORT_CTRL_SPEED_2500M		4

/* PORT_STATUSm: base 0x80200, stride 4, one per MAC */
#define YT9215_PORT_STATUS_REG(macid)	(0x80200 + ((macid) * 0x4))
#define   YT9215_PORT_STATUS_LINK		BIT(8)
#define   YT9215_PORT_STATUS_DUPLEX		BIT(7)
#define   YT9215_PORT_STATUS_RX_FC		BIT(6)
#define   YT9215_PORT_STATUS_TX_FC		BIT(5)
#define   YT9215_PORT_STATUS_SPEED_MASK		(0x7U << 0)

/* VLAN: per-port default CVID, admission and egress tagging */
#define YT9215_PORT_VLAN_CTRLN_REG(macid)	(0x230010 + ((macid) * 0x4))
#define   YT9215_PORT_VLAN_CTRLN_CVID_OFFSET	6
#define   YT9215_PORT_VLAN_CTRLN_CVID_MASK	(0xfffU << 6)
#define YT9215_PORT_VLAN_CTRL1N_REG(macid)	(0x230080 + ((macid) * 0x4))
#define   YT9215_PORT_VLAN_CTRL1N_CTAG_AFT_MASK	(0x3U << 0)
#define   YT9215_PORT_VLAN_CTRL1N_STAG_AFT_MASK	(0x3U << 2)
#define   YT9215_VLAN_AFT_ALL			0
#define YT9215_EGR_PORT_VLAN_CTRLN_REG(macid)	(0x100080 + ((macid) * 0x4))
#define   YT9215_EGR_PORT_VLAN_CTRLN_CTAG_MODE_OFFSET	12
#define   YT9215_EGR_PORT_VLAN_CTRLN_CTAG_MODE_MASK	(0x7U << 12)
#define   YT9215_EGR_CTAG_MODE_ENTRY_BASED	5
#define YT9215_TPID_PROFILE0_REG	0x210000
#define YT9215_EGR_TPID_PROFILE0_REG	0x100300
#define YT9215_TPID_8100		0x8100
#define YT9215_PARSER_PORT_CTRLN_REG(macid)	(0x210010 + ((macid) * 0x4))
#define   YT9215_PARSER_PORT_CTRLN_CTAG_TPID_MASK	(0xfU << 0)
#define   YT9215_PARSER_TPID_PROFILE0			0x1
#define YT9215_EGR_PORT_CTRLN_REG(macid)	(0x100000 + ((macid) * 0x4))
#define   YT9215_EGR_PORT_CTRLN_CTAG_TPID_SEL_MASK	(0x3U << 4)

/* L2 VLAN table: base 0x188000, stride 8 (two words per VID) */
#define YT9215_VLAN_TBL_REG(vid)	(0x188000 + ((vid) * 0x8))
#define   YT9215_VLAN_TBL_MEMBER_OFFSET	7
#define   YT9215_VLAN_TBL_MEMBER_MASK	(0x7ffU << 7)
#define   YT9215_VLAN_TBL_UNTAG_OFFSET	8
#define   YT9215_VLAN_TBL_UNTAG_MASK	(0x7ffU << 8)

/* Internal PHY indirect access (INT_IF) */
#define YT9215_INT_IF_FRAME_CTRL	0xf0000
#define YT9215_INT_IF_ADDR_CTRL		0xf0004
#define YT9215_INT_IF_DATA_0		0xf0008
#define YT9215_INT_IF_DATA_1		0xf000c
#define   YT9215_INT_IF_OP_WRITE		1
#define   YT9215_INT_IF_OP_READ			2
#define   YT9215_INT_IF_POLL_MAX		100

/* Port layout and defaults */
#define YT9215_MAX_MAC			11	/* MAC0-10 */
#define YT9215_USER_PORTS		4	/* MAC0-3: internal GE PHYs */
#define YT9215_DEFAULT_CPU_PORT		4	/* MAC4: CPU/RGMII uplink */
#define YT9215_DEFAULT_SERDES_PORT	8	/* MAC8: SerDes uplink */
#define YT9215_DEFAULT_MDIO_ADDR	0x1d
#define YT9215_DEFAULT_SERDES_SPEED	2500

/* Reset pulse width */
#define YT9215_RESET_ASSERT_MS		10
#define YT9215_RESET_DEASSERT_MS	50

#define YT9215_CHIP_PROBE_RETRY		3

typedef struct {
	u32 mdio_addr;		/* Clause-22 SMI address of the switch */
	int chip_detect;	/* non-zero once the chip has been probed */
	u32 cpu_port;		/* CPU port MAC id (default MAC4) */
	u32 serdes_port;	/* SerDes uplink MAC id (default MAC8) */
	u32 serdes_speed;	/* SerDes speed: 1000 (SGMII) or 2500 */
	u32 last_link;		/* cached per-port link bitmap */
} ipq_yt9215_swt_cfg_t;

/* Switch side (ipq_yt9215.c) */
extern void ipq_yt9215_switch_reset(int gpio);
extern int ipq_yt9215_switch_init(ipq_yt9215_swt_cfg_t *swt_cfg);
extern int ipq_yt9215_link_update(ipq_yt9215_swt_cfg_t *swt_cfg);

#endif /* _IPQ_YT9215_H */