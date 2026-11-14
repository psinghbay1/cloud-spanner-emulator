//
// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

package gateway

import (
	"slices"
	"testing"
)

func TestEmulatorArgsForwardsPruningFlagsWhenSet(t *testing.T) {
	gw := New(Options{
		FrontendAddress:    "localhost:9010",
		SessionIdleTimeout: "30m",
		OperationRetention: "10m",
	})
	args := gw.emulatorArgs()
	for _, want := range []string{"--session_idle_timeout=30m", "--operation_retention=10m"} {
		if !slices.Contains(args, want) {
			t.Errorf("emulatorArgs() = %v, missing %q", args, want)
		}
	}
}

func TestEmulatorArgsOmitsPruningFlagsWhenUnset(t *testing.T) {
	gw := New(Options{FrontendAddress: "localhost:9010"})
	for _, arg := range gw.emulatorArgs() {
		for _, prefix := range []string{"--session_idle_timeout", "--operation_retention"} {
			if len(arg) >= len(prefix) && arg[:len(prefix)] == prefix {
				t.Errorf("emulatorArgs() forwarded %q although it was unset", arg)
			}
		}
	}
}
