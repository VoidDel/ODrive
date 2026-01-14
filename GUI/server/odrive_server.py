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
    while globals()['inUse']:
        time.sleep(0.1)
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
    session['samplingEnabled'] = False
    emit('samplingDisabled')

@socketio.on('sampledVarNames')
def sampledVarNames(message):
    session['sampledVars'] = message
    print(session['sampledVars'])
    
@socketio.on('startSampling')
def sendSamples(message):
    print(session['samplingEnabled'])
    while session['samplingEnabled']:
        emit('sampledData', json.dumps(getSampledData(session['sampledVars'])))
        time.sleep(0.02)

@socketio.on('message')
def handle_message(message):
    print(message)
    emit('response', 'hello from server!')

@socketio.on('getODrives')
def get_odrives(data):
    # spinlock
    while globals()['inUse']:
        time.sleep(0.1)

    globals()['inUse'] = True
    odriveDict = {}
    #for (index, odrv) in enumerate(globals()['odrives']):
    #    odriveDict["odrive" + str(index)] = dictFromRO(odrv)
    for key in globals()['odrives_status'].keys():
        if globals()['odrives_status'][key] == True:
            odriveDict[key] = dictFromRO(globals()['odrives'][key])
    globals()['inUse'] = False
    emit('odrives', json.dumps(odriveDict))

@socketio.on('getProperty')
def get_property(message):
    # message is dict natively
    # will be {"path": "odriveX.axisY.blah.blah"}
    while globals()['inUse']:
        time.sleep(0.1)
    if globals()['odrives_status'][message["path"].split('.')[0]]:
        globals()['inUse'] = True
        val = getVal(globals()['odrives'], message["path"].split('.'))
        globals()['inUse'] = False
        emit('ODriveProperty', json.dumps({"path": message["path"], "val": val}))

@socketio.on('setProperty')
def set_property(message):
    # message is {"path":, "val":, "type":}
    while globals()['inUse']:
        time.sleep(0.1)
    globals()['inUse'] = True
    print("From setProperty event handler: " + str(message))
    postVal(globals()['odrives'], message["path"].split('.'), message["val"], message["type"])
    val = getVal(globals()['odrives'], message["path"].split('.'))
    globals()['inUse'] = False
    emit('ODriveProperty', json.dumps({"path": message["path"], "val": val}))

@socketio.on('callFunction')
def call_function(message):
    # message is {"path"}, no args yet (do we know which functions accept arguments from the odrive tree directly?)
    while globals()['inUse']:
        time.sleep(0.1)
    print("From callFunction event handler: " + str(message))
    globals()['inUse'] = True
    callFunc(globals()['odrives'], message["path"].split('.'))
    globals()['inUse'] = False

@app.route('/', methods=['GET'])
def home():
    return "<h1>ODrive GUI Server</h1>"

def await_if_coroutine(value):
    """
    If value is a coroutine, await it and return the result.
    Otherwise, return the value directly.
    """
    if inspect.iscoroutine(value):
        # Run the coroutine in a new event loop
        try:
            loop = asyncio.get_event_loop()
            if loop.is_running():
                # If loop is already running, create a new one
                loop = asyncio.new_event_loop()
                asyncio.set_event_loop(loop)
            return loop.run_until_complete(value)
        except:
            # Fallback: create new event loop
            loop = asyncio.new_event_loop()
            asyncio.set_event_loop(loop)
            result = loop.run_until_complete(value)
            return result
    return value

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
    odrv = None
    try:
        odrv = keyList.pop(0)
        RO = odrives[odrv]
        keyList[-1] = '_' + keyList[-1] + '_property'
        for key in keyList:
            RO = getattr(RO, key)
        if argType == "number":
            await_if_coroutine(RO.exchange(float(value)))
        elif argType == "boolean":
            await_if_coroutine(RO.exchange(value))
        elif argType == "string":
            if value == "Infinity":
                await_if_coroutine(RO.exchange(math.inf))
            elif value == "-Infinity":
                await_if_coroutine(RO.exchange(-math.inf))
        else:
            pass # dont support that type yet
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
        odrv = keyList.pop(0)
        RO = odrives[odrv]
        keyList[-1] = '_' + keyList[-1] + '_property'
        for key in keyList:
            RO = getattr(RO, key)
        retVal = await_if_coroutine(RO.read())
        if retVal == math.inf:
            retVal = "Infinity"
        elif retVal == -math.inf:
            retVal = "-Infinity"
        return retVal
    except Exception as ex:
        # Check if it's a connection/object lost error
        error_str = str(ex).lower()
        if 'lost' in error_str or 'disconnect' in error_str or 'connection' in error_str:
            if odrv:
                handle_disconnect(odrv)
        print("exception in getVal: ", traceback.format_exc())
        return 0

def getSampledData(vars):
    #use getVal to populate a dict
    #return a dict {path:value}
    samples = {}
    for path in vars["paths"]:
        keys = path.split('.')
        samples[path] = getVal(globals()['odrives'], keys)

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
    # spinlock
    globals()['inUse'] = False

    # Initialize fibre logger and event if available (API changed in newer versions)
    try:
        if hasattr(fibre, 'Logger'):
            log = fibre.Logger(verbose=False)
        if hasattr(fibre, 'Event'):
            shutdown = fibre.Event()
    except Exception as e:
        print(f"Warning: Could not initialize fibre Logger/Event: {e}")

    socketio.run(app, host='0.0.0.0', port=5000)
