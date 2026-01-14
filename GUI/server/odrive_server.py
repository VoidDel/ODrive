import sys
import flask
import os
from flask import make_response, request, jsonify, session
from flask_socketio import SocketIO, send, emit
from flask_cors import CORS
from engineio.payload import Payload
import json
import time
import argparse
import logging
import math
import traceback
import asyncio
import inspect
import threading

# interface for odrive GUI to get data from odrivetool

# Redirect stdout and stderr to a log file for debugging
log_file = open(os.path.join(os.path.dirname(__file__), 'server_debug.log'), 'w')
sys.stdout = log_file
sys.stderr = log_file

old_print = print
def print(*args, **kwargs):
    kwargs.pop('flush', False)
    old_print(*args, file=sys.stdout, **kwargs)
    sys.stdout.flush()

app = flask.Flask(__name__)
# disable logging, very noisy!
log = logging.getLogger('werkzeug')
log.disabled = True
app.config['SECRET_KEY'] = 'secret'
app.config.update(
    SESSION_COOKIE_SECURE=True,
    SESSION_COOKIE_HTTPONLY=True,
    SESSION_COOKIE_SAMESITE='None'
)
CORS(app, support_credentials=True)
Payload.max_decode_packets = 500
socketio = SocketIO(app, cors_allowed_origins="*", async_mode = "threading")

#def get_odrive():
#    globals()['odrives'] = []
#    globals()['odrives'].append(odrive.find_any())
#    globals()['odrives'][0].__channel__._channel_broken.subscribe(lambda: handle_disconnect())
#    print("odrives found")
#    socketio.emit('odrive-found')

def discovered_device(device):
    # when device is discovered, add it to list of serial numbers and global odrive list
    try:
        # direct property access is usually supported in newer odrive versions
        # or fall back to hex(device.serial_number)
        serial_number = str(device.serial_number)
    except Exception as e:
        print(f"Error getting serial number: {e}")
        serial_number = "unknown_serial"

    if serial_number in globals()['discovered_devices']:
        index = globals()['discovered_devices'].index(serial_number)
    else:
        globals()['discovered_devices'].append(serial_number)
        index = len(globals()['discovered_devices']) - 1
    odrive_name = "odrive" + str(index)

    # add to list of odrives
    print(f"discovered_device: adding {odrive_name}, lock exists: {'odrive_lock' in globals()}")
    if 'odrive_lock' in globals():
        with globals()['odrive_lock']:
            globals()['odrives'][odrive_name] = device
            globals()['odrives_status'][odrive_name] = True
            print("Found " + str(serial_number))
    else:
        print("WARNING: Lock not initialized yet!")
        globals()['odrives'][odrive_name] = device
        globals()['odrives_status'][odrive_name] = True
        print("Found " + str(serial_number))

    # tell GUI the status of known ODrives
    socketio.emit('odrives-status', json.dumps(globals()['odrives_status']))
    # triggers a getODrives socketio message
    socketio.emit('odrive-found')

# Global flag to control discovery thread
discovery_thread = None

def discovery_loop():
    print("Discovery loop started")
    while True:
        # Check if we already have a connected device to avoid spamming find_any
        # User said "only connect the first one", so if we have one, we chill.
        connected_count = sum(1 for status in globals()['odrives_status'].values() if status)
        if connected_count > 0:
            time.sleep(2)
            continue
            
        print("Scanning for ODrive...")
        try:
            # timeout=5 allows the loop to check for exit conditions or re-eval status
            device = odrive.find_any(timeout=5)
            if device:
                print("ODrive detected via find_any()")
                discovered_device(device)
                # After finding one, we might want to wait a bit or keep monitoring
        except Exception as e:
            # find_any might throw if timeout or other issues, though timeout usually returns None? 
            # odrive 0.6.x find_any raises TimeoutError? No, usually just blocks or returns None if timeout arg supported? 
            # Actually odrive.find_any(timeout=...) returns None or raises? 
            # If it throws, we catch it.
            pass
        
        time.sleep(1)

def start_discovery():
    global discovery_thread
    if discovery_thread is None:
        print("starting disco thread...")
        import threading
        discovery_thread = threading.Thread(target=discovery_loop, daemon=True)
        discovery_thread.start()

def handle_disconnect(odrive_name):
    print("lost odrive")
    globals()['odrives_status'][odrive_name] = False
    # emit the whole list of odrive statuses
    # in the GUI, mark and use status as ODrive state.
    socketio.emit('odrives-status', json.dumps(globals()['odrives_status']))

@socketio.on('findODrives')
def getODrives(message):
    print("looking for odrive")
    start_discovery()

@socketio.on('enableSampling')
def enableSampling(message):
    print("sampling enabled")
    session['samplingEnabled'] = True
    emit('samplingEnabled')

@socketio.on('stopSampling')
def stopSampling(message):
    print("sampling disabled")
    session['samplingEnabled'] = False
    emit('samplingDisabled')

@socketio.on('sampledVarNames')
def sampledVarNames(message):
    session['sampledVars'] = message
    print(f"sampledVars set: {session['sampledVars']}")

@socketio.on('startSampling')
def sendSamples(message):
    print(f"startSampling: samplingEnabled={session.get('samplingEnabled', False)}")
    # Reset debug counter for timing measurements
    globals()['_sampling_debug_counter'] = 0
    # Simple blocking loop - Flask-SocketIO threading mode handles this properly
    iteration = 0
    last_log_time = time.time()
    loop_start_time = time.time()
    while session.get('samplingEnabled', False):
        try:
            iter_start = time.time()
            iteration += 1

            # Detailed logging for first iteration only
            if iteration == 1:
                print(f"First iteration timing:")

            data = getSampledData(session.get('sampledVars', {'paths': []}))

            if iteration == 1:
                emit_start = time.time()

            emit('sampledData', json.dumps(data))

            if iteration == 1:
                emit_time = (time.time() - emit_start) * 1000
                iter_time = (time.time() - iter_start) * 1000
                print(f"  Iteration time: {iter_time:.0f}ms")
                print(f"  Sampling rate: ~{1000/iter_time:.1f}Hz")

            # Log statistics every 10 seconds
            current_time = time.time()
            if current_time - last_log_time >= 10.0:
                elapsed = current_time - loop_start_time
                actual_hz = iteration / elapsed if elapsed > 0 else 0
                print(f"Sampling: {iteration} samples in {elapsed:.0f}s = {actual_hz:.1f}Hz")
                last_log_time = current_time

            # No sleep - run at maximum possible speed
            # (limited by ODrive USB communication speed)

        except Exception as e:
            print(f"Error in sampling loop at iteration {iteration}: {e}")
            import traceback
            traceback.print_exc()
            break

    print(f"startSampling loop ended after {iteration} iterations")

@socketio.on('message')
def handle_message(message):
    print(message)
    emit('response', 'hello from server!')

@socketio.on('getODrives')
def get_odrives(data):
    print(">>> getODrives called, acquiring lock...")
    with globals()['odrive_lock']:
        print(">>> getODrives: lock acquired")
        odriveDict = {}
        #for (index, odrv) in enumerate(globals()['odrives']):
        #    odriveDict["odrive" + str(index)] = dictFromRO(odrv)
        for key in globals()['odrives_status'].keys():
            if globals()['odrives_status'][key] == True:
                print(f">>> getODrives: building dict for {key}")
                odriveDict[key] = dictFromRO(globals()['odrives'][key])
        print(">>> getODrives: emitting odrives")
        emit('odrives', json.dumps(odriveDict))
    print(">>> getODrives: lock released, done")

@socketio.on('getProperty')
def get_property(message):
    # message is dict natively
    # will be {"path": "odriveX.axisY.blah.blah"}

    # Skip if write operation in progress
    if globals().get('pause_sampling', False):
        return

    if globals()['odrives_status'][message["path"].split('.')[0]]:
        # Read operations don't need lock - fibre library is thread-safe for reads
        val = getVal(globals()['odrives'], message["path"].split('.'))
        emit('ODriveProperty', json.dumps({"path": message["path"], "val": val}))

@socketio.on('setProperty')
def set_property(message):
    # message is {"path":, "val":, "type":}
    print(f"setProperty called: {message}")
    try:
        # Signal reads to pause briefly during write
        globals()['pause_sampling'] = True

        # Acquire lock for exclusive write access
        with globals()['odrive_lock']:
            print("Lock acquired, writing...")
            postVal(globals()['odrives'], message["path"].split('.'), message["val"], message["type"])
            print(f"Write completed")

        # Release pause flag immediately after write, don't wait for read
        globals()['pause_sampling'] = False
        print("setProperty completed, pause_sampling released")

        # Read back value without holding the pause flag
        # This may be slow but won't block sampling
        val = getVal(globals()['odrives'], message["path"].split('.'))
        emit('ODriveProperty', json.dumps({"path": message["path"], "val": val}))
        print(f"Read back value: {val}")

    except Exception as e:
        globals()['pause_sampling'] = False
        print(f"setProperty error: {e}")
        import traceback
        traceback.print_exc()

@socketio.on('callFunction')
def call_function(message):
    # message is {"path"}, no args yet (do we know which functions accept arguments from the odrive tree directly?)
    with globals()['odrive_lock']:
        callFunc(globals()['odrives'], message["path"].split('.'))

@app.route('/', methods=['GET'])
def home():
    return "<h1>ODrive GUI Server</h1>"

def await_if_coroutine(value, timeout=2.0):
    """
    If value is a coroutine, await it and return the result.
    Otherwise, return the value directly.
    """
    if inspect.iscoroutine(value):
        # Ensure this thread has an event loop before awaiting.
        loop = ensure_event_loop()
        try:
            result = loop.run_until_complete(asyncio.wait_for(value, timeout=timeout))
            return result
        except asyncio.TimeoutError:
            print(f"WARNING: Coroutine timed out after {timeout}s")
            return None
        except Exception as e:
            print(f"WARNING: Coroutine failed: {e}")
            return None
    return value

_thread_local = threading.local()

def ensure_event_loop():
    """Ensure this thread has an asyncio event loop set."""
    loop = getattr(_thread_local, 'loop', None)
    if loop is None or loop.is_closed():
        loop = asyncio.new_event_loop()
        _thread_local.loop = loop
    asyncio.set_event_loop(loop)
    return loop

def is_odrive_object(obj):
    """Check if an object is an ODrive remote object using duck typing."""
    # Check for common ODrive object attributes
    obj_type = type(obj).__name__
    module_name = type(obj).__module__

    # Remote objects typically have these characteristics
    if 'RemoteObject' in obj_type or 'remote' in module_name.lower():
        return True

    # Check if it has typical ODrive object structure
    if hasattr(obj, '__dict__') or (hasattr(obj, '__dir__') and len(dir(obj)) > 10):
        # Not a simple value type
        try:
            # Try to check if it's not a basic Python type
            if not isinstance(obj, (int, float, str, bool, list, dict, type(None))):
                # Has attributes like an ODrive object
                return any(not attr.startswith('_') for attr in dir(obj))
        except:
            pass

    return False

def is_odrive_property(obj):
    """Check if an object is an ODrive property using duck typing."""
    obj_type = type(obj).__name__

    # Check for property-like characteristics
    if 'Property' in obj_type or 'property' in obj_type.lower():
        return True

    # Check for methods that properties typically have
    if hasattr(obj, 'get_value') or hasattr(obj, 'read') or hasattr(obj, 'exchange'):
        return True

    return False

def dictFromRO(RO):
    """Create dict from an ODrive RemoteObject that's suitable for sending as JSON."""
    returnDict = {}
    ensure_event_loop()

    for key in dir(RO):
        if key.startswith('_'):
             continue

        try:
            v = getattr(RO, key)
        except Exception:
            continue

        # Skip if it's a callable (function/method)
        if hasattr(v, '__call__'):
            returnDict[key] = "function"
            continue

        # Check if it's a simple value type
        if isinstance(v, (int, float, str, bool)):
            # Direct value
            val = v
            if val == float("inf"):
                val = "Infinity"
            elif val == float("-inf"):
                val = "-Infinity"
            else:
                val = str(val)

            # Determine type
            if isinstance(v, bool):
                _type = "boolean"
            elif isinstance(v, int):
                _type = "number"
            elif isinstance(v, float):
                _type = "float"
            else:
                _type = "string"

            returnDict[key] = {
                "val": val,
                "readonly": False,
                "type": _type
            }
            continue

        # Check if it's an ODrive property
        if is_odrive_property(v):
            try:
                # Try different methods to read the property value
                if hasattr(v, 'get_value'):
                    val = await_if_coroutine(v.get_value())
                elif hasattr(v, 'read'):
                    val = await_if_coroutine(v.read())
                elif hasattr(v, '__call__'):
                    # Some properties might be callable
                    try:
                        val = await_if_coroutine(v())
                    except:
                        continue
                else:
                    # Can't read this property
                    continue

                # Check for infinity
                if val == float("inf"):
                    val = "Infinity"
                elif val == float("-inf"):
                    val = "-Infinity"
                else:
                    val = str(val)

                # Determine type
                _type = "float"
                if isinstance(val, bool):
                    _type = "boolean"
                elif isinstance(val, int):
                    _type = "number"

                returnDict[key] = {
                    "val": val,
                    "readonly": False,
                    "type": _type
                }
            except Exception as e:
                pass
            continue

        # Check if it's an ODrive remote object (recurse)
        if is_odrive_object(v):
            try:
                returnDict[key] = dictFromRO(v)
            except:
                pass

    return returnDict

def postVal(odrives, keyList, value, argType):
    # expect a list of keys in the form of ["key1", "key2", "keyN"]
    # "key1" will be "odriveN"
    # like this: postVal(odrives, ["odrive0","axis0","config","calibration_lockin","accel"], 17.0)
    print(f"postVal: keyList={keyList}, value={value}, argType={argType}")
    odrv = None
    try:
        odrv = keyList.pop(0)
        RO = odrives[odrv]
        keyList[-1] = '_' + keyList[-1] + '_property'
        print(f"postVal: Getting property path: {keyList}")
        for key in keyList:
            RO = getattr(RO, key)
        print(f"postVal: Got property object, writing value...")
        if argType == "number":
            ensure_event_loop()
            result = RO.write(float(value))
            print(f"postVal: write() returned {type(result).__name__}")
            await_if_coroutine(result, timeout=1.0)  # 1 second timeout for write
        elif argType == "boolean":
            ensure_event_loop()
            await_if_coroutine(RO.write(value), timeout=1.0)
        elif argType == "string":
            if value == "Infinity":
                ensure_event_loop()
                await_if_coroutine(RO.write(math.inf), timeout=1.0)
            elif value == "-Infinity":
                ensure_event_loop()
                await_if_coroutine(RO.write(-math.inf), timeout=1.0)
        else:
            pass # dont support that type yet
        print(f"postVal: Write completed")
    except Exception as ex:
        # Check if it's a connection/object lost error
        error_str = str(ex).lower()
        if 'lost' in error_str or 'disconnect' in error_str or 'connection' in error_str:
            if odrv:
                handle_disconnect(odrv)
        print("exception in postVal: ", traceback.format_exc())

def getVal(odrives, keyList):
    odrv = None
    try:
        original_path = '.'.join(keyList)  # Save before modifying keyList
        odrv = keyList.pop(0)
        RO = odrives[odrv]
        keyList[-1] = '_' + keyList[-1] + '_property'
        for key in keyList:
            RO = getattr(RO, key)
        ensure_event_loop()
        # Use very short timeout for sampling (0.1s)
        retVal = await_if_coroutine(RO.read(), timeout=0.1)
        if retVal == math.inf:
            retVal = "Infinity"
        elif retVal == -math.inf:
            retVal = "-Infinity"
        return retVal
    except asyncio.TimeoutError:
        print(f"Timeout reading {original_path}")
        return 0
    except Exception as ex:
        # Check if it's a connection/object lost error
        error_str = str(ex).lower()
        if 'lost' in error_str or 'disconnect' in error_str or 'connection' in error_str:
            if odrv:
                handle_disconnect(odrv)
        print(f"exception in getVal({original_path}): {ex}")
        return 0

def getSampledData(vars):
    #use getVal to populate a dict
    #return a dict {path:value}
    start_time = time.time()
    samples = {}

    # Skip sampling if a write operation is in progress
    if globals().get('pause_sampling', False):
        return samples

    # Read operations don't need lock - fibre library is thread-safe for reads
    # Only write operations (setProperty) need exclusive access
    paths = vars.get("paths", [])

    # Remove duplicates to avoid redundant reads
    unique_paths = list(dict.fromkeys(paths))

    # Log timing for first sample only
    debug_first = globals().get('_sampling_debug_counter', 0) == 0

    for i, path in enumerate(unique_paths):
        try:
            path_start = time.time()
            keys = path.split('.')
            val = getVal(globals()['odrives'], keys)
            samples[path] = val
            path_time = (time.time() - path_start) * 1000

            if debug_first:
                print(f"    {path}: {path_time:.0f}ms")
        except Exception as e:
            print(f"Error reading {path}: {e}")
            samples[path] = 0  # Return 0 on error to keep sampling going

    total_time = (time.time() - start_time) * 1000
    if debug_first:
        print(f"  Total read time: {total_time:.0f}ms for {len(unique_paths)} properties")
        globals()['_sampling_debug_counter'] = 1

    return samples

def callFunc(odrives, keyList):
    odrv = None
    try:
        #index = int(''.join([char for char in keyList.pop(0) if char.isnumeric()]))
        odrv = keyList.pop(0)
        RO = odrives[odrv]
        for key in keyList:
            RO = getattr(RO, key)
        if hasattr(RO, '__call__'):
            ensure_event_loop()
            await_if_coroutine(RO.__call__())
    except Exception as ex:
        # Check if it's a connection/object lost error
        error_str = str(ex).lower()
        if 'lost' in error_str or 'disconnect' in error_str or 'connection' in error_str:
            if odrv:
                handle_disconnect(odrv)
        print("fcn call failed: ", traceback.format_exc())

if __name__ == "__main__":
    print("args from python: " + str(sys.argv[1:0]))
    #print(sys.argv[1:])
    # try to import based on command line arguments or config file

    for optPath in sys.argv[1:]:
        print("adding " + str(optPath.rstrip()) + " to import path for odrive_server.py")
        sys.path.insert(0,optPath.rstrip())

    import odrive
    import odrive.utils # for dump_errors()
    import fibre

    # global for holding references to all connected odrives
    globals()['odrives'] = {}
    # global dict {'odriveX': True/False} where True/False reflects status of connection
    # on handle_disconnect, set it to False. On connection, set it to True
    globals()['odrives_status'] = {}
    globals()['discovered_devices'] = []
    # Thread lock for ODrive access
    globals()['odrive_lock'] = threading.Lock()
    # Flag to pause sampling when write operations are in progress
    globals()['pause_sampling'] = False

    # Initialize fibre logger and event if available (API changed in newer versions)
    try:
        if hasattr(fibre, 'Logger'):
            log = fibre.Logger(verbose=False)
        if hasattr(fibre, 'Event'):
            shutdown = fibre.Event()
    except Exception as e:
        print(f"Warning: Could not initialize fibre Logger/Event: {e}")

    socketio.run(app, host='0.0.0.0', port=5000, allow_unsafe_werkzeug=True)
