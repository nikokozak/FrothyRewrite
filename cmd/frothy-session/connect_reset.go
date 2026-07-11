// Reset detection. ESP ROM bootloaders print a small set of
// line-start prefixes when the chip resets. A line counts as part of
// the banner only if its trimmed form starts with one of those
// prefixes; a prefix appearing mid-text in normal output does not match.
//
// Sample banner from the ESP32 ROM (POWERON path, DevKit V-5 target),
// sourced from the ESP32 TRM "Boot Mode" section, the ESP-IDF startup
// docs, and the prefixes idf_monitor.py matches.
//
//	ets Jul 29 2019 12:21:46
//
//	rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
//	configsip: 0, SPIWP:0xee
//	clk_drv:0x00,q_drv:0x00,d_drv:0x00,cs0_drv:0x00,hd_drv:0x00,wp_drv:0x00
//	mode:DIO, clock div:2
//	load:0x3fff0030,len:1184
//	load:0x40078000,len:13076
//	load:0x40080400,len:3076
//	entry 0x400805f0

package main

import "strings"

var resetBannerPrefixes = []string{
	"ets ",
	"ESP-ROM:",
	"rst:0x",
	"boot:0x",
	"load:0x",
	"entry 0x",
}

func isResetBannerLine(line string) bool {
	trimmed := strings.TrimLeft(line, " \t")
	for _, prefix := range resetBannerPrefixes {
		if strings.HasPrefix(trimmed, prefix) {
			return true
		}
	}
	return false
}
