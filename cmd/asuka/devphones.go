package main

import (
	"errors"
	"fmt"
	"os"
	"slices"
	"strings"
)

// phonePlatforms splits --platform's ios,android: the phones asuka dev
// runs the app on at once. One platform is nil (devOn's own).
func phonePlatforms(platform string) ([]string, error) {
	if !strings.Contains(platform, ",") {
		return nil, nil
	}
	var ps []string
	for _, p := range strings.Split(platform, ",") {
		p = strings.TrimSpace(p)
		if p != platformIOS && p != platformAndroid {
			return nil, fmt.Errorf("--platform %s: the phones (ios, android) run together; %q runs alone", platform, p)
		}
		if slices.Contains(ps, p) {
			return nil, fmt.Errorf("--platform %s: %s twice", platform, p)
		}
		ps = append(ps, p)
	}
	return ps, nil
}

// devPhones runs the app on the iOS simulator and an Android device or
// emulator at once (asuka dev --platform ios,android), as Expo's dev
// server serves both: one loop, the first platform's, compiles each save
// and sends it to every player connected, each its platform's module;
// the others' players are built, launched and relaunched (the app's
// settings, assets or native code changed) beside the first's, and stop
// with it.
func devPhones(dir string, o *devOptions, platforms []string, stop <-chan os.Signal) error {
	if o.device != "" {
		return errors.New("--device picks one platform's device: give it with one --platform (the phones together run on the booted simulator and the first device adb lists)")
	}
	type phone struct {
		app  *asukaApp
		name string
		p    devPlayer
	}
	var loop *devLoop
	var phones []phone
	for _, platform := range platforms {
		app, err := loadApp(dir, platform)
		if err != nil {
			return err
		}
		if loop == nil {
			if loop, err = newDevLoop(app, "127.0.0.1:0"); err != nil {
				return err
			}
		}
		var p devPlayer
		var name string
		switch platform {
		case platformIOS:
			tc, err := newIOSToolchain(o.verbose)
			if err != nil {
				return err
			}
			work, err := workDir(app, "ios")
			if err != nil {
				return err
			}
			sim, err := pickSimulator("")
			if err != nil {
				return err
			}
			name = sim.Name
			p = simulatorPlayer(app, tc, work, sim, loop)
		case platformAndroid:
			tc, err := newAndroidToolchain(o.verbose)
			if err != nil {
				return err
			}
			work, err := workDir(app, "android")
			if err != nil {
				return err
			}
			serial, err := tc.pickDevice("")
			if err != nil {
				return err
			}
			name = serial
			if p, err = androidPlayer(app, tc, work, serial, loop); err != nil {
				return err
			}
		}
		fmt.Printf("asuka: %s on %s\n", app.name, name)
		phones = append(phones, phone{app, name, p})
	}
	first := phones[0]
	both := devPlayer{
		launch: func() error {
			if err := first.p.launch(); err != nil {
				return err
			}
			for _, ph := range phones[1:] {
				// The loop's compile is the first's: the others take its C
				// (the app's externs, the same for each) and the settings
				// as they are now.
				if cfg, err := loadConfig(ph.app.dir); err == nil {
					ph.app.cfg, ph.app.bundleID = cfg, cfg.ID
					ph.app.refresh()
				}
				ph.app.c = loop.app.c
				if err := ph.p.launch(); err != nil {
					fmt.Fprintf(os.Stderr, "asuka: %s: %v\n", ph.name, err)
				}
			}
			return nil
		},
		stop: func() {
			for _, ph := range phones {
				if ph.p.stop != nil {
					ph.p.stop()
				}
			}
		},
		done: first.p.done,
	}
	loop.flows = o.flows
	return loop.run(both, stop, o.once)
}
