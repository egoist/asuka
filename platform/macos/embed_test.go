package macos

import (
	"bytes"
	"os"
	"testing"
)

// Typed native modules' runtime, and the Swift the Mac's packages build
// in (StoreKit's, iCloud's), are one file each in the iOS host and here.
func TestSwiftShared(t *testing.T) {
	for _, name := range []string{"NativeModules.swift", "Purchase.swift", "ICloud.swift"} {
		ios, err := os.ReadFile("../ios/Sources/AsukaHost/" + name)
		if err != nil {
			t.Fatal(err)
		}
		mac, err := Files.ReadFile("Sources/AsukaMac/" + name)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(ios, mac) {
			t.Errorf("platform/macos/Sources/AsukaMac/%s isn't the iOS host's: copy it", name)
		}
	}
}
