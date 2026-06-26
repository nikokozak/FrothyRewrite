//go:build darwin || linux

package main

import "testing"

func TestParseLsofHolders(t *testing.T) {
	got := parseLsofHolders("p123\ncfrothy\np456\ncCoolTerm\n")
	if len(got) != 2 {
		t.Fatalf("holders = %#v, want 2", got)
	}
	if got[0].pid != 123 || got[0].command != "frothy" {
		t.Fatalf("first holder = %#v", got[0])
	}
	if got[1].pid != 456 || got[1].command != "CoolTerm" {
		t.Fatalf("second holder = %#v", got[1])
	}
}

func TestFormatSerialPortBusyMessage(t *testing.T) {
	cases := []struct {
		name    string
		holders []serialPortHolder
		want    string
	}{
		{
			name: "frothy holder",
			holders: []serialPortHolder{
				{pid: 12, command: "/usr/local/bin/frothy", frothy: true},
			},
			want: "port /dev/cu.test is in use by another frothy (pid 12); run 'frothy stop' to free it",
		},
		{
			name: "non frothy holder",
			holders: []serialPortHolder{
				{pid: 34, command: "/Applications/CoolTerm.app/Contents/MacOS/CoolTerm"},
			},
			want: "port /dev/cu.test is held by /Applications/CoolTerm.app/Contents/MacOS/CoolTerm (pid 34), which is not Frothy; close it and retry",
		},
		{
			name: "no holders",
			want: "port /dev/cu.test is in use",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := formatSerialPortBusyMessage("/dev/cu.test", tc.holders); got != tc.want {
				t.Fatalf("message = %q, want %q", got, tc.want)
			}
		})
	}
}

func TestIsFrothyHolder(t *testing.T) {
	if !isFrothyHolder("/tmp/build/frothy", "/elsewhere/frothy") {
		t.Fatal("frothy basename was not classified as Frothy")
	}
	if !isFrothyHolder("/tmp/current", "/tmp/current") {
		t.Fatal("current executable path was not classified as Frothy")
	}
	if isFrothyHolder("/Applications/CoolTerm", "/tmp/frothy") {
		t.Fatal("non-Frothy command classified as Frothy")
	}
}
