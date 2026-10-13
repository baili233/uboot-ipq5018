/*
 * Motorcomm YT9215 / YT9218 unmanaged Ethernet switch driver for U-Boot.
 *
 * The switch hangs off the SoC MDIO bus at a single Clause-22 address and
 * exposes its whole register space through a 4-slot SMI window.  This driver
 * ports the register access, chip probe, internal-PHY setup, port/SerDes
 * configuration, default-VLAN programming and link monitor of the Linux
 * swconfig driver (yt9215_core.c / yt9215_reg.c) into U-Boot, so the IPQ5018
 * GMAC driver can bring the switch up before the kernel takes over.
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <command.h>
#include <malloc.h>
#include <net.h>
#include <asm-generic/errno.h>
#include <asm/io.h>
#include "ipq_yt9215.h"

extern int ipq_mdio_read(int mii_id, int regnum, ushort *data);
extern int ipq_mdio_write(int mii_id, int regnum, u16 value);

/* ---- Clause-22 SMI register access ---- */
static int yt9215_reg_read(u32 addr, u32 reg, u32 *val)
{
	u16 hi, lo;

	ipq_mdio_write(addr, YT9215_SMI_ADDR_READ, (u16)(reg >> 16));
	ipq_mdio_write(addr, YT9215_SMI_ADDR_READ, (u16)(reg & 0xffff));

	ipq_mdio_read(addr, YT9215_SMI_DATA_READ, &hi);
	ipq_mdio_read(addr, YT9215_SMI_DATA_READ, &lo);

	if (val)
		*val = ((u32)hi << 16) | lo;
	return 0;
}

static int yt9215_reg_write(u32 addr, u32 reg, u32 val)
{
	ipq_mdio_write(addr, YT9215_SMI_ADDR_WRITE, (u16)(reg >> 16));
	ipq_mdio_write(addr, YT9215_SMI_ADDR_WRITE, (u16)(reg & 0xffff));

	ipq_mdio_write(addr, YT9215_SMI_DATA_WRITE, (u16)(val >> 16));
	ipq_mdio_write(addr, YT9215_SMI_DATA_WRITE, (u16)(val & 0xffff));
	return 0;
}

static int yt9215_reg_set(u32 addr, u32 reg, u32 mask, u32 set)
{
	u32 val;

	yt9215_reg_read(addr, reg, &val);
	val = (val & ~mask) | (set & mask);
	return yt9215_reg_write(addr, reg, val);
}

/* ---- Internal PHY indirect access (INT_IF) ---- */
static int yt9215_phy_write(u32 addr, u8 phy, u32 reg, u16 val)
{
	u32 addr_ctrl;
	int i;

	if (reg > 0x1f) {
		yt9215_phy_write(addr, phy, 0x1e, (u16)reg);
		return yt9215_phy_write(addr, phy, 0x1f, val);
	}

	addr_ctrl = ((u32)(phy & 0x1f) << 21) |
		    ((u32)(reg & 0x1f) << 16) |
		    (YT9215_INT_IF_OP_WRITE << 2);

	yt9215_reg_write(addr, YT9215_INT_IF_ADDR_CTRL, addr_ctrl);
	yt9215_reg_write(addr, YT9215_INT_IF_DATA_0, val);
	yt9215_reg_write(addr, YT9215_INT_IF_FRAME_CTRL, 1);

	for (i = 0; i < YT9215_INT_IF_POLL_MAX; i++) {
		u32 status;

		yt9215_reg_read(addr, YT9215_INT_IF_FRAME_CTRL, &status);
		if (!status)
			return 0;
		udelay(1);
	}

#ifdef CONFIG_YT9215_DEBUG
	printf("YT9215: PHY write timeout phy=%d reg=0x%x\n", phy, reg);
#endif
	return -ETIMEDOUT;
}

static int yt9215_phy_read(u32 addr, u8 phy, u32 reg, u16 *val)
{
	u32 addr_ctrl, data;
	int i;

	if (reg > 0x1f) {
		yt9215_phy_write(addr, phy, 0x1e, (u16)reg);
		return yt9215_phy_read(addr, phy, 0x1f, val);
	}

	addr_ctrl = ((u32)(phy & 0x1f) << 21) |
		    ((u32)(reg & 0x1f) << 16) |
		    (YT9215_INT_IF_OP_READ << 2);

	yt9215_reg_write(addr, YT9215_INT_IF_ADDR_CTRL, addr_ctrl);
	yt9215_reg_write(addr, YT9215_INT_IF_FRAME_CTRL, 1);

	for (i = 0; i < YT9215_INT_IF_POLL_MAX; i++) {
		u32 status;

		yt9215_reg_read(addr, YT9215_INT_IF_FRAME_CTRL, &status);
		if (!status) {
			yt9215_reg_read(addr, YT9215_INT_IF_DATA_1, &data);
			if (val)
				*val = (u16)data;
			return 0;
		}
		udelay(1);
	}

#ifdef CONFIG_YT9215_DEBUG
	printf("YT9215: PHY read timeout phy=%d reg=0x%x\n", phy, reg);
#endif
	return -ETIMEDOUT;
}

/* ---- Chip probe / reset ---- */
static int yt9215_chip_probe(u32 addr, u32 *chip_id)
{
	u32 val = 0;

	yt9215_reg_read(addr, YT9215_CHIP_ID_REG, &val);
	if (chip_id)
		*chip_id = (val >> 16) & 0xffff;

	if (((val >> 16) & 0xffff) != YT9215_CHIP_ID_9215 &&
	    ((val >> 16) & 0xffff) != YT9215_CHIP_ID_9218)
		return -1;

	return 0;
}

void ipq_yt9215_switch_reset(int gpio)
{
	if (!gpio)
		return;

	gpio_direction_output(gpio, 0);
	mdelay(YT9215_RESET_ASSERT_MS);
	gpio_direction_output(gpio, 1);
	mdelay(YT9215_RESET_DEASSERT_MS);
}

/* ---- Internal PHY setup (port of the vendor int_yt861x_init) ---- */
static int yt9215_phy_init(u32 addr, u32 chip_id)
{
	u16 reg;
	int phy;

	for (phy = 1; phy <= YT9215_USER_PORTS; phy++) {
		if (chip_id == YT9215_CHIP_ID_9218) {
			yt9215_phy_read(addr, phy, 0x50, &reg);
			reg &= 0xf3ff;
			yt9215_phy_write(addr, phy, 0x50, reg);
		}

		yt9215_phy_read(addr, phy, 0x408, &reg);
		reg &= 0xfffe;
		yt9215_phy_write(addr, phy, 0x408, reg);

		yt9215_phy_read(addr, phy, 0x29, &reg);
		reg &= ~0x3f;
		reg |= 0x8;
		yt9215_phy_write(addr, phy, 0x29, reg);

		yt9215_phy_read(addr, phy, 0x3a9, &reg);
		reg &= ~0x3f;
		reg |= 0x17;
		yt9215_phy_write(addr, phy, 0x3a9, reg);

		yt9215_phy_write(addr, phy, 0x00, 0x1240);
	}

	return 0;
}

/* ---- Port configuration ---- */
static void yt9215_port_set_user(u32 addr, u32 mac)
{
	/* Auto-negotiate with the internal PHY, enable both MAC directions. */
	yt9215_reg_set(addr, YT9215_PORT_CTRL_REG(mac),
		       YT9215_PORT_CTRL_FLOW_LINK_AN |
		       YT9215_PORT_CTRL_AN_LINK_EN |
		       YT9215_PORT_CTRL_RXMAC_EN |
		       YT9215_PORT_CTRL_TXMAC_EN,
		       YT9215_PORT_CTRL_FLOW_LINK_AN |
		       YT9215_PORT_CTRL_AN_LINK_EN |
		       YT9215_PORT_CTRL_RXMAC_EN |
		       YT9215_PORT_CTRL_TXMAC_EN);
}

static void yt9215_port_set_cpu(u32 addr, u32 mac)
{
	/*
	 * The RGMII CPU port (MAC4) has its own internal PHY, exactly like the
	 * user ports, so it auto-negotiates with the SoC's GE PHY rather than
	 * being forced.
	 */
	yt9215_port_set_user(addr, mac);
}

static void yt9215_port_set_serdes(u32 addr, u32 mac, u32 speed)
{
	u32 val, serdes = mac - YT9215_DEFAULT_SERDES_PORT;
	u32 ctrl = YT9215_SGMII_CTRL_REG(serdes);

	/*
	 * EXTIF0 as SerDes (SGMII/2500BASE-X), EXTIF1 as xMII.  The SerDes
	 * link to the SoC UNIPHY is forced up so the CPU uplink carries the
	 * user traffic straight away.
	 */
	yt9215_reg_set(addr, YT9215_INTERFACE_CTRL_REG,
		       YT9215_INTERFACE_CTRL_EXTIF0_EN,
		       YT9215_INTERFACE_CTRL_EXTIF0_EN);
	yt9215_reg_set(addr, YT9215_CHIP_INTERFACE_SEL_REG,
		       YT9215_EXTIF_SEL_EXTIF1_MASK,
		       YT9215_EXTIF_SEL_EXTIF1_XMII);
	yt9215_reg_set(addr, YT9215_CHIP_INTERFACE_SEL_REG,
		       YT9215_EXTIF_SEL_EXTIF0_MASK,
		       YT9215_EXTIF_SEL_EXTIF0_SERDES);

	val = YT9215_SGMII_CTRL_LINK | YT9215_SGMII_CTRL_DUPLEX_FULL;
	if (speed == 1000)
		val |= YT9215_SGMII_CTRL_MODE_SGMII_PHY << YT9215_SGMII_CTRL_MODE_OFFSET;
	else
		val |= YT9215_SGMII_CTRL_MODE_2500BASEX << YT9215_SGMII_CTRL_MODE_OFFSET;
	val |= (speed == 1000) ? YT9215_SGMII_CTRL_SPEED_1000
			       : YT9215_SGMII_CTRL_SPEED_2500;
	yt9215_reg_write(addr, ctrl, val);

	/* Force the SerDes MAC itself to full-duplex at the uplink speed. */
	yt9215_reg_set(addr, YT9215_PORT_CTRL_REG(mac),
		       YT9215_PORT_CTRL_FLOW_LINK_AN |
		       YT9215_PORT_CTRL_AN_LINK_EN |
		       YT9215_PORT_CTRL_DUPLEX_MODE |
		       YT9215_PORT_CTRL_RX_FC_EN |
		       YT9215_PORT_CTRL_TX_FC_EN |
		       YT9215_PORT_CTRL_RXMAC_EN |
		       YT9215_PORT_CTRL_TXMAC_EN |
		       YT9215_PORT_CTRL_SPEED_MODE_MASK,
		       YT9215_PORT_CTRL_DUPLEX_MODE |
		       YT9215_PORT_CTRL_RX_FC_EN |
		       YT9215_PORT_CTRL_TX_FC_EN |
		       YT9215_PORT_CTRL_RXMAC_EN |
		       YT9215_PORT_CTRL_TXMAC_EN |
		       (speed == 1000 ? YT9215_PORT_CTRL_SPEED_1000M
				      : YT9215_PORT_CTRL_SPEED_2500M));
}

/* ---- VLAN: one untagged fabric on VID 1 ---- */
static void yt9215_vlan_setup(u32 addr, u32 cpu_port, u32 serdes_port)
{
	u32 member = 0, untag = 0;
	u32 mac;

	/*
	 * All four user ports plus both CPU uplinks form one VLAN, entirely
	 * untagged: U-Boot's network stack does not strip a VLAN tag, so the
	 * switch must hand the CPU ports plain Ethernet frames.
	 */
	for (mac = 0; mac < YT9215_USER_PORTS; mac++)
		member |= BIT(mac);
	member |= BIT(cpu_port) | BIT(serdes_port);
	untag = member;

	yt9215_reg_write(addr, YT9215_TPID_PROFILE0_REG, YT9215_TPID_8100);
	yt9215_reg_write(addr, YT9215_EGR_TPID_PROFILE0_REG, YT9215_TPID_8100);

	/* Recognise 0x8100 as a C-tag, admit everything, tag by VLAN table. */
	for (mac = 0; mac < YT9215_MAX_MAC; mac++) {
		yt9215_reg_set(addr, YT9215_PARSER_PORT_CTRLN_REG(mac),
			       YT9215_PARSER_PORT_CTRLN_CTAG_TPID_MASK,
			       YT9215_PARSER_TPID_PROFILE0);
		yt9215_reg_set(addr, YT9215_PORT_VLAN_CTRL1N_REG(mac),
			       YT9215_PORT_VLAN_CTRL1N_CTAG_AFT_MASK |
			       YT9215_PORT_VLAN_CTRL1N_STAG_AFT_MASK,
			       YT9215_VLAN_AFT_ALL);
		yt9215_reg_set(addr, YT9215_EGR_PORT_CTRLN_REG(mac),
			       YT9215_EGR_PORT_CTRLN_CTAG_TPID_SEL_MASK, 0);
		yt9215_reg_set(addr, YT9215_EGR_PORT_VLAN_CTRLN_REG(mac),
			       YT9215_EGR_PORT_VLAN_CTRLN_CTAG_MODE_MASK,
			       (u32)YT9215_EGR_CTAG_MODE_ENTRY_BASED <<
			       YT9215_EGR_PORT_VLAN_CTRLN_CTAG_MODE_OFFSET);

		/* Default PVID 1 on every port. */
		yt9215_reg_set(addr, YT9215_PORT_VLAN_CTRLN_REG(mac),
			       YT9215_PORT_VLAN_CTRLN_CVID_MASK,
			       1 << YT9215_PORT_VLAN_CTRLN_CVID_OFFSET);
	}

	/* VID 1 membership / untag map (word1 write latches the entry). */
	yt9215_reg_write(addr, YT9215_VLAN_TBL_REG(1),
			 member << YT9215_VLAN_TBL_MEMBER_OFFSET);
	yt9215_reg_write(addr, YT9215_VLAN_TBL_REG(1) + 4,
			 untag << YT9215_VLAN_TBL_UNTAG_OFFSET);
}

/* ---- Full switch bring-up ---- */
int ipq_yt9215_switch_init(ipq_yt9215_swt_cfg_t *cfg)
{
	u32 chip_id = 0, mac;
	int retry;

	if (!cfg)
		return -1;

	if (!cfg->mdio_addr)
		cfg->mdio_addr = YT9215_DEFAULT_MDIO_ADDR;
	if (!cfg->cpu_port)
		cfg->cpu_port = YT9215_DEFAULT_CPU_PORT;
	if (!cfg->serdes_port)
		cfg->serdes_port = YT9215_DEFAULT_SERDES_PORT;
	if (!cfg->serdes_speed)
		cfg->serdes_speed = YT9215_DEFAULT_SERDES_SPEED;

	for (retry = 0; retry < YT9215_CHIP_PROBE_RETRY; retry++) {
		if (yt9215_chip_probe(cfg->mdio_addr, &chip_id) == 0)
			break;
		mdelay(100);
	}
	if (retry == YT9215_CHIP_PROBE_RETRY) {
		printf("YT9215: chip probe failed at mdio 0x%x\n",
		       cfg->mdio_addr);
		return -1;
	}

	printf("YT9215: chip id 0x%04x at mdio 0x%x\n", chip_id,
	       cfg->mdio_addr);

	/* Enable the MIB counters. */
	yt9215_reg_set(cfg->mdio_addr, YT9215_GLOBAL_CTRL1_REG,
		       YT9215_GLOBAL_CTRL1_MIB_EN, YT9215_GLOBAL_CTRL1_MIB_EN);

	yt9215_phy_init(cfg->mdio_addr, chip_id);

	for (mac = 0; mac < YT9215_USER_PORTS; mac++)
		yt9215_port_set_user(cfg->mdio_addr, mac);

	yt9215_port_set_cpu(cfg->mdio_addr, cfg->cpu_port);
	yt9215_port_set_serdes(cfg->mdio_addr, cfg->serdes_port,
			       cfg->serdes_speed);
	yt9215_vlan_setup(cfg->mdio_addr, cfg->cpu_port, cfg->serdes_port);

	cfg->chip_detect = 1;
	cfg->last_link = 0;

	printf("YT9215: switch ready (cpu mac%u, serdes mac%u %uM)\n",
	       cfg->cpu_port, cfg->serdes_port, cfg->serdes_speed);
	return 0;
}

/* ---- Link monitor ---- */
static const char *yt9215_speed_str(u32 sp)
{
	switch (sp) {
	case 0:  return "10M";
	case 1:  return "100M";
	case 2:  return "1G";
	case 4:  return "2.5G";
	default: return "???";
	}
}

int ipq_yt9215_link_update(ipq_yt9215_swt_cfg_t *cfg)
{
	u32 status, link = 0, changed, mac;
	int any_up = 0;

	if (!cfg || !cfg->chip_detect)
		return 1;

	for (mac = 0; mac < YT9215_USER_PORTS; mac++) {
		if (yt9215_reg_read(cfg->mdio_addr, YT9215_PORT_STATUS_REG(mac),
				    &status) < 0)
			continue;
		if (status & YT9215_PORT_STATUS_LINK)
			link |= BIT(mac);
	}

	changed = link ^ cfg->last_link;
	if (changed) {
		for (mac = 0; mac < YT9215_USER_PORTS; mac++) {
			if (!(changed & BIT(mac)))
				continue;
			if (link & BIT(mac)) {
				yt9215_reg_read(cfg->mdio_addr,
						YT9215_PORT_STATUS_REG(mac),
						&status);
				printf("YT9215: Port%u Link Up - %s %s\n", mac,
				       yt9215_speed_str(status &
						YT9215_PORT_STATUS_SPEED_MASK),
				       (status & YT9215_PORT_STATUS_DUPLEX) ?
				       "Full" : "Half");
			} else {
				printf("YT9215: Port%u Link Down\n", mac);
			}
		}
		cfg->last_link = link;
	}

	if (link)
		any_up = 1;

	return any_up ? 0 : 1;
}

#ifdef CONFIG_CMD_YT9215
static int do_yt9215(cmd_tbl_t *cmdtp, int flag, int argc, char *const argv[])
{
	u32 addr = YT9215_DEFAULT_MDIO_ADDR, reg = 0, val = 0;

	if (argc < 2)
		return CMD_RET_USAGE;

	/* read/write take an explicit address, id/status default to 0x1d. */
	if (!strcmp(argv[1], "read")) {
		if (argc != 4)
			return CMD_RET_USAGE;
		addr = simple_strtoul(argv[2], NULL, 16);
		reg = simple_strtoul(argv[3], NULL, 16);
		yt9215_reg_read(addr, reg, &val);
		printf("YT9215[0x%x] 0x%08x: 0x%08x\n", addr, reg, val);
		return CMD_RET_SUCCESS;
	}

	if (!strcmp(argv[1], "write")) {
		if (argc != 5)
			return CMD_RET_USAGE;
		addr = simple_strtoul(argv[2], NULL, 16);
		reg = simple_strtoul(argv[3], NULL, 16);
		val = simple_strtoul(argv[4], NULL, 16);
		yt9215_reg_write(addr, reg, val);
		return CMD_RET_SUCCESS;
	}

	if (argc >= 3)
		addr = simple_strtoul(argv[2], NULL, 16);

	if (!strcmp(argv[1], "id")) {
		yt9215_reg_read(addr, YT9215_CHIP_ID_REG, &val);
		printf("YT9215[0x%x] chip id: 0x%08x\n", addr, val);
		return CMD_RET_SUCCESS;
	}

	if (!strcmp(argv[1], "status")) {
		for (reg = 0; reg < YT9215_USER_PORTS; reg++) {
			yt9215_reg_read(addr, YT9215_PORT_STATUS_REG(reg), &val);
			printf("YT9215[0x%x] port%u status: 0x%08x %s\n",
			       addr, reg, val,
			       (val & YT9215_PORT_STATUS_LINK) ? "up" : "down");
		}
		return CMD_RET_SUCCESS;
	}

	return CMD_RET_USAGE;
}

U_BOOT_CMD(
	yt9215, 5, 1, do_yt9215,
	"YT9215 switch register access",
	"id [addr]              - read the chip id\n"
	"read  <addr> <reg>     - read a switch register\n"
	"write <addr> <reg> <val> - write a switch register\n"
	"status [addr]          - print user-port link status"
);
#endif