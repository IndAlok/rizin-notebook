/// \file scripting.go
/// \brief Goja-based JS engine with rizin.cmd/cmdj and console.* APIs.

package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/dop251/goja"
	"golang.org/x/sync/semaphore"
)

// scriptTimeout is the maximum execution time for a single script.
const scriptTimeout = 5 * time.Minute

/// \brief Goja JS runtime with Rizin pipe integration.
type JavaScript struct {
	semaphore *semaphore.Weighted
	runtime   *goja.Runtime
	rizin     *Rizin
	output    strings.Builder
}

/// \brief Executes a Rizin command with color disabled for clean script output.
func rizinCmd(command string, rz *Rizin) (string, error) {
	if _, err := rz.exec("e scr.color=0"); err != nil {
		return "", err
	}
	result, err := rz.exec(command)
	// Restore color regardless of command success.
	rz.exec("e scr.color=3")
	return result, err
}

/// \brief Converts a Go value to string (JSON-pretty for arrays/maps).
func convertValue(ivalue interface{}) string {
	switch value := ivalue.(type) {
	case []interface{}:
		bytes, _ := json.MarshalIndent(value, "", "\t")
		return string(bytes)
	case map[string]interface{}:
		bytes, _ := json.MarshalIndent(value, "", "\t")
		return string(bytes)
	default:
		return fmt.Sprintf("%v", value)
	}
}

/// \brief Creates the JS engine and registers rizin.* and console.* APIs.
func NewJavaScript() *JavaScript {
	runtime := goja.New()
	if runtime == nil {
		fmt.Println("error: cannot create JavaScript runtime")
		return nil
	}

	sem := semaphore.NewWeighted(1)
	js := &JavaScript{semaphore: sem, runtime: runtime, rizin: nil}

	// Register the rizin API object.
	rizinAPI := map[string]interface{}{}
	rizinAPI["cmd"] = func(args ...interface{}) goja.Value {
		if js.rizin == nil {
			panic(js.runtime.ToValue("Rizin pipe is closed."))
		}
		if len(args) < 1 {
			panic(js.runtime.ToValue("No string was passed."))
		}
		cmdStr, ok := args[0].(string)
		if !ok {
			panic(js.runtime.ToValue("input is not a string."))
		}
		result, err := rizinCmd(cmdStr, js.rizin)
		if err != nil {
			panic(js.runtime.ToValue(err.Error()))
		}
		return js.runtime.ToValue(result)
	}

	rizinAPI["cmdj"] = func(args ...interface{}) goja.Value {
		if js.rizin == nil {
			panic(js.runtime.ToValue("Rizin pipe is closed."))
		}
		if len(args) < 1 {
			panic(js.runtime.ToValue("No string was passed."))
		}
		cmdStr, ok := args[0].(string)
		if !ok {
			panic(js.runtime.ToValue("input is not a string."))
		}
		result, err := rizinCmd(cmdStr, js.rizin)
		if err != nil {
			panic(js.runtime.ToValue(err.Error()))
		}
		var data interface{}
		if jsonErr := json.Unmarshal([]byte(result), &data); jsonErr != nil {
			panic(js.runtime.ToValue(jsonErr.Error()))
		}
		return js.runtime.ToValue(data)
	}

	// Register the console API object.
	consoleAPI := map[string]interface{}{}

	// writeArgs writes all arguments to js.output separated by spaces.
	writeArgs := func(prefix string, args []interface{}) {
		if len(prefix) > 0 {
			js.output.WriteString(prefix)
			js.output.WriteString(" ")
		}
		for i, value := range args {
			js.output.WriteString(convertValue(value))
			if i+1 < len(args) {
				js.output.WriteString(" ")
			}
		}
		js.output.WriteString("\n")
	}

	consoleAPI["log"] = func(args ...interface{}) {
		writeArgs("", args)
	}

	consoleAPI["warn"] = func(args ...interface{}) {
		writeArgs("[WARN]", args)
	}

	consoleAPI["error"] = func(args ...interface{}) {
		writeArgs("[ERROR]", args)
	}

	consoleAPI["info"] = func(args ...interface{}) {
		writeArgs("[INFO]", args)
	}

	consoleAPI["clear"] = func() {
		js.output.Reset()
	}

	consoleAPI["table"] = func(args ...interface{}) {
		if len(args) < 1 {
			return
		}
		bytes, err := json.MarshalIndent(args[0], "", "  ")
		if err != nil {
			js.output.WriteString(fmt.Sprintf("[TABLE] %v\n", args[0]))
			return
		}
		js.output.WriteString(string(bytes))
		js.output.WriteString("\n")
	}

	runtime.Set("rizin", rizinAPI)
	runtime.Set("console", consoleAPI)
	return js
}

/// \brief Runs a script with semaphore-limited concurrency and timeout.
func (js *JavaScript) exec(script string, rz *Rizin) (string, error) {
	if !js.semaphore.TryAcquire(1) {
		return "", errors.New("a script is already running")
	}
	defer js.semaphore.Release(1)

	js.rizin = rz
	js.output.Reset()

	timer := time.AfterFunc(scriptTimeout, func() {
		js.runtime.Interrupt("The script execution has timed out.")
	})
	defer timer.Stop()

	_, err := js.runtime.RunScript("script.js", script)
	result := js.output.String()

	// Clean up state for next execution.
	js.rizin = nil
	js.output.Reset()

	return result, err
}
