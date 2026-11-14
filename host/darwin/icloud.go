//go:build darwin && !ios

package darwin

import (
	"errors"
	"unsafe"

	"github.com/ebitengine/purego"

	spec "github.com/egoist/asuka/internal/icloudspec"
)

// asuka/sdk/icloud on a Mac: the iOS host's Swift (platform/macos's
// ICloud.swift, built into AsukaNatives.dylib for an app that imports the
// package: the CLI's featurePermissions), for NSMetadataQuery's and
// NSFileCoordinator's blocks and the key-value store's notifications. The
// module hands each call to the library's asuka_icloud_call, and its
// answer comes back through a callback, on the main thread. Without the
// library's (an app run with go run, not built), there's no iCloud: its
// entitlements need the app signed with a provisioning profile.

var swiftICloud struct {
	call    func(id uint64, method *byte, args unsafe.Pointer, n int)
	ok      bool
	next    uint64
	replies map[uint64]func([]byte, error)
}

func init() {
	moduleHooks = append(moduleHooks, func(h *Host, m map[string]map[string]method) {
		m["icloud"] = icloudModule()
	})
}

// loadSwiftICloud finds the library's iCloud functions, if it has them.
func loadSwiftICloud(lib uintptr) {
	if _, err := purego.Dlsym(lib, "asuka_icloud_call"); err != nil {
		return
	}
	var boot func(reply uintptr)
	purego.RegisterLibFunc(&boot, lib, "asuka_icloud_boot")
	purego.RegisterLibFunc(&swiftICloud.call, lib, "asuka_icloud_call")
	swiftICloud.ok = true
	swiftICloud.replies = map[uint64]func([]byte, error){}
	boot(purego.NewCallback(swiftICloudReply))
}

// swiftICloudReply answers a call (a purego callback, on the main thread).
func swiftICloudReply(id uint64, ok int32, p *byte, n int) {
	reply := swiftICloud.replies[id]
	if reply == nil {
		return
	}
	delete(swiftICloud.replies, id)
	var b []byte
	if n > 0 && p != nil {
		b = append([]byte(nil), unsafe.Slice(p, n)...)
	}
	if ok == 0 {
		reply(nil, errors.New(string(b)))
		return
	}
	reply(b, nil)
}

func icloudModule() map[string]method {
	unbuilt := spec.Unavailable("iCloud needs the app built (asuka dev or asuka build), whose Swift has it, and signed with a provisioning profile")
	m := map[string]method{}
	for _, name := range spec.Methods {
		cname := append([]byte(name), 0)
		m[name] = func(args []byte, reply func([]byte, error)) {
			if !swiftICloud.ok {
				unbuilt[name](args, reply)
				return
			}
			swiftICloud.next++
			id := swiftICloud.next
			swiftICloud.replies[id] = reply
			var p unsafe.Pointer
			if len(args) > 0 {
				p = unsafe.Pointer(unsafe.SliceData(args))
			}
			swiftICloud.call(id, unsafe.SliceData(cname), p, len(args))
		}
	}
	return m
}
