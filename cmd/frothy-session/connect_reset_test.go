package main

import "testing"

func TestIsResetBannerLine(t *testing.T) {
	cases := []struct {
		name string
		line string
		want bool
	}{
		{"first banner line", "ets Jul 29 2019 12:21:46", true},
		{"rst and boot same line", "rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)", true},
		{"boot on own line", "boot:0x13 (SPI_FAST_FLASH_BOOT)", true},
		{"load segment", "load:0x3fff0030,len:1184", true},
		{"entry line", "entry 0x400805f0", true},
		{"leading spaces", "  ets Jul 29 2019 12:21:46", true},
		{"leading tab", "\tentry 0x400805f0", true},
		{"empty line", "", false},
		{"plain ok", "ok", false},
		{"value response", "value is 42", false},
		{"rst mid line", "see rst:0x1 for details", false},
		{"boot mid line", "trace shows boot:0x13", false},
		{"ets missing trailing space", "etset Jul", false},
		{"rst wrong case", "rst:0X1", false},
		{"entry missing space", "entry0x400805f0", false},
		{"esp-rom variant out of scope", "ESP-ROM:esp32s3-20210327", false},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := isResetBannerLine(tc.line); got != tc.want {
				t.Errorf("isResetBannerLine(%q) = %v, want %v", tc.line, got, tc.want)
			}
		})
	}
}
