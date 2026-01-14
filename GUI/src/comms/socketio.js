import io from 'socket.io-client';

let socket = undefined;
let url = 'http://localhost:5000';

function initSocket(url) {
    socket = io(url);
}

export function setUrl(path) {
    if (socket) {
        socket.close();
        socket = undefined;
    }
    url = path;
    initSocket(url);
}

export function closeSocket() {
    socket.close();
    socket = undefined;
}

export function addEventListener(event) {
    if (!socket) {
        initSocket(url);
    }
    socket.on(event.type, event.callback);
}

export function sendEvent(event) {
    if (!socket) {
        console.error('Socket not initialized!');
        initSocket(url);
    }
    if (!socket.connected) {
        console.error('Socket not connected! Current URL:', url);
    }
    // Only log setProperty events to reduce noise
    if (event.type === 'setProperty' || event.type === 'callFunction') {
        console.log('Sending socket event:', event.type, 'data:', event.data);
    }
    socket.emit(event.type, event.data);
}