'use strict';

const fs = require('fs');
const path = require('path');

const html = fs.readFileSync(path.resolve(
	__dirname,
	'../package/nes-emulator/src/play.html'
), 'utf8');
const scripts = Array.from(
	html.matchAll(/<script>([\s\S]*?)<\/script>/g),
	match => match[1]
);
if (scripts.length !== 1)
	throw new Error(`expected one inline game-client script, got ${scripts.length}`);
const gameStageMarkup = html.match(
	/<section id="game-stage"[\s\S]*?<\/section>/
);
if (!gameStageMarkup || !gameStageMarkup[0].includes('id="screen"') ||
    !gameStageMarkup[0].includes('id="fps-osd"') ||
	!gameStageMarkup[0].includes('data-touch-controls="visible"') ||
	!gameStageMarkup[0].includes('data-control="up"') ||
	!gameStageMarkup[0].includes('id="btn-fullscreen"')) {
	throw new Error('fullscreen game stage does not contain video and touch controls');
}
if (html.includes('id="fps-counter"') || html.includes('#fps-counter') ||
    html.includes('const fpsNode') || /<output[^>]+fps/i.test(gameStageMarkup[0])) {
	throw new Error('FPS OSD exposed a browser widget');
}
if (scripts[0].includes("'FPS: '") || scripts[0].includes('fillText('))
	throw new Error('FPS OSD rendered a label instead of only the numeric value');
if (!scripts[0].includes('Math.max(0, fpsOsd.width - 40) + 2') ||
    !scripts[0].includes('const originY = 6;')) {
	throw new Error('FPS OSD left the FCEUX top-right display zone');
}
const styleMarkup = html.match(/<style>([\s\S]*?)<\/style>/);
if (!styleMarkup)
	throw new Error('game client stylesheet is missing');
const flatStyle = styleMarkup[1].replace(/\/\*[\s\S]*?\*\//g, '');
const styleRules = Array.from(
	flatStyle.matchAll(/([^{}]+)\{([^{}]*)\}/g),
	match => ({
		selectors: match[1].split(',').map(selector => selector.trim()),
		declarations: match[2]
	})
);
for (const rule of styleRules) {
	const fullscreenSelector = rule.selectors.some(selector =>
		selector.includes('#game-stage') &&
		(selector.includes('fullscreen-active') ||
		 selector.includes(':fullscreen') ||
		 selector.includes(':-webkit-full-screen'))
	);
	if (fullscreenSelector && rule.selectors.length !== 1) {
		throw new Error(
			'fullscreen selectors must not share an unforgiving selector list'
		);
	}
}
function runtimeFullscreenDeclarations(selector) {
	const rules = styleRules.filter(rule => rule.selectors.includes(selector));
	if (rules.length !== 1 || rules[0].selectors.length !== 1) {
		throw new Error(
			`runtime fullscreen selector must be an independent rule: ${selector}`
		);
	}
	return rules[0].declarations;
}
const fullscreenRootDeclarations = runtimeFullscreenDeclarations(
	'#game-stage.fullscreen-active'
);
const fullscreenFrameDeclarations = runtimeFullscreenDeclarations(
	'#game-stage.fullscreen-active .screen-frame'
);
const fullscreenToolbarDeclarations = runtimeFullscreenDeclarations(
	'#game-stage.fullscreen-active .display-toolbar'
);
const standardFullscreenToolbarDeclarations = runtimeFullscreenDeclarations(
	'#game-stage:fullscreen .display-toolbar'
);
const webkitFullscreenToolbarDeclarations = runtimeFullscreenDeclarations(
	'#game-stage:-webkit-full-screen .display-toolbar'
);
const visibleTouchToolbarDeclarations = [
	runtimeFullscreenDeclarations(
		'#game-stage.fullscreen-active[data-touch-controls="visible"] .display-toolbar'
	),
	runtimeFullscreenDeclarations(
		'#game-stage:fullscreen[data-touch-controls="visible"] .display-toolbar'
	),
	runtimeFullscreenDeclarations(
		'#game-stage:-webkit-full-screen[data-touch-controls="visible"] .display-toolbar'
	)
];
const fullscreenTouchDeclarations = runtimeFullscreenDeclarations(
	'#game-stage.fullscreen-active .touch-controls'
);
const hiddenTouchRules = styleRules.filter(rule =>
	rule.selectors.includes('.touch-controls[hidden]')
);
const sharedCanvasRules = styleRules.filter(rule =>
	rule.selectors.includes('#screen') && rule.selectors.includes('#fps-osd')
);
const fpsOsdRules = styleRules.filter(rule =>
	rule.selectors.length === 1 && rule.selectors[0] === '#fps-osd'
);
const screenFrameRules = styleRules.filter(rule =>
	rule.selectors.length === 1 && rule.selectors[0] === '.screen-frame'
);
const fullscreenToolbarPositions = [
	fullscreenToolbarDeclarations,
	standardFullscreenToolbarDeclarations,
	webkitFullscreenToolbarDeclarations
];
if (!/position\s*:\s*fixed\s*;/.test(fullscreenRootDeclarations) ||
    !/overflow\s*:\s*hidden\s*;/.test(fullscreenRootDeclarations) ||
    !/justify-content\s*:\s*center\s*;/.test(fullscreenRootDeclarations) ||
    !/flex\s*:\s*none\s*;/.test(fullscreenFrameDeclarations) ||
    !/position\s*:\s*absolute\s*;/.test(fullscreenToolbarDeclarations) ||
    !/position\s*:\s*absolute\s*;/.test(fullscreenTouchDeclarations)) {
	throw new Error('runtime fullscreen CSS does not isolate the picture from overlays');
}
if (fullscreenToolbarPositions.some(declarations =>
		!/top\s*:\s*auto\s*;/.test(declarations) ||
		!/right\s*:\s*auto\s*;/.test(declarations) ||
		!/bottom\s*:\s*max\(8px,\s*env\(safe-area-inset-bottom\)\)\s*;/.test(declarations) ||
		!/left\s*:\s*50%\s*;/.test(declarations) ||
		!/transform\s*:\s*translateX\(-50%\)\s*;/.test(declarations)
	) || visibleTouchToolbarDeclarations.some(declarations =>
		!/top\s*:\s*max\(8px,\s*env\(safe-area-inset-top\)\)\s*;/.test(declarations) ||
		!/right\s*:\s*auto\s*;/.test(declarations) ||
		!/bottom\s*:\s*auto\s*;/.test(declarations) ||
		!/left\s*:\s*max\(8px,\s*env\(safe-area-inset-left\)\)\s*;/.test(declarations) ||
		!/transform\s*:\s*none\s*;/.test(declarations) ||
		!/flex-direction\s*:\s*column\s*;/.test(declarations) ||
		!/align-items\s*:\s*stretch\s*;/.test(declarations) ||
		!/width\s*:\s*calc\(100vw\s*-\s*64px\)\s*;/.test(declarations) ||
		!/max-width\s*:\s*13rem\s*;/.test(declarations) ||
		!/box-sizing\s*:\s*border-box\s*;/.test(declarations)
	)) {
	throw new Error('fullscreen toolbar can obscure the FCEUX FPS OSD or touch controls');
}
for (const viewportWidth of [240, 256, 320, 390, 844, 1920]) {
	const compactToolbarRight = 8 + Math.min(13 * 16, viewportWidth - 64);
	const fpsZoneLeft = viewportWidth * (256 - 40) / 256;
	if (compactToolbarRight > fpsZoneLeft) {
		throw new Error(
			`visible-touch fullscreen toolbar reaches the FPS zone at ${viewportWidth}px`
		);
	}
}
if (hiddenTouchRules.length !== 1 ||
    !/display\s*:\s*none\s*!important\s*;?/.test(
	    hiddenTouchRules[0].declarations
    )) {
	throw new Error('hidden touch controls can be overridden by fullscreen CSS');
}
if (sharedCanvasRules.length !== 1 ||
    !/position\s*:\s*absolute\s*;/.test(sharedCanvasRules[0].declarations) ||
    !/width\s*:\s*100%\s*;/.test(sharedCanvasRules[0].declarations) ||
    !/height\s*:\s*100%\s*;/.test(sharedCanvasRules[0].declarations) ||
    !/border\s*:\s*0\s*;/.test(sharedCanvasRules[0].declarations) ||
    fpsOsdRules.length !== 1 ||
    !/background\s*:\s*transparent\s*;/.test(fpsOsdRules[0].declarations) ||
    !/pointer-events\s*:\s*none\s*;/.test(fpsOsdRules[0].declarations) ||
    screenFrameRules.length !== 1 ||
    !/overflow\s*:\s*hidden\s*;/.test(screenFrameRules[0].declarations) ||
    !/border\s*:\s*2px\s+solid\s+#333\s*;/.test(
	    screenFrameRules[0].declarations
    ) ||
    !/border\s*:\s*0\s*;/.test(fullscreenFrameDeclarations)) {
	throw new Error('FPS OSD did not use a transparent pixel renderer');
}

let drawImageCount = 0;
const drawnWidths = [];
let putImageDataCount = 0;
let fillRectCount = 0;
let lastRawPixel = [];
let fpsOsdAlpha = null;
let fpsOsdCurrentRects = [];
class MockElement {
	constructor(id) {
		this.id = id;
		this.value = '';
		this.textContent = '';
		this.title = '';
		this.disabled = false;
		this.hidden = false;
		this.files = null;
		this.dataset = {};
		this.width = 256;
		this.height = 240;
		this.clientWidth = 0;
		this.clientHeight = 0;
		this.options = [];
		this.attributes = new Map();
		this.listeners = new Map();
		const styles = new Map();
		this.style = {
			setProperty: (name, value) => styles.set(name, String(value)),
			removeProperty: name => styles.delete(name),
			getPropertyValue: name => styles.get(name) || ''
		};
		this.computedStyle = {
			paddingLeft: '0px',
			paddingRight: '0px',
			paddingTop: '0px',
			paddingBottom: '0px'
		};
		const classes = new Set();
		this.classList = {
			add: (...names) => names.forEach(name => classes.add(name)),
			remove: (...names) => names.forEach(name => classes.delete(name)),
			contains: name => classes.has(name)
		};
	}

	addEventListener(name, callback) {
		if (!this.listeners.has(name))
			this.listeners.set(name, []);
		this.listeners.get(name).push(callback);
	}
	dispatch(name, event = {}) {
		for (const callback of this.listeners.get(name) || [])
			callback(event);
	}
	setAttribute(name, value) {
		this.attributes.set(name, String(value));
	}
	getAttribute(name) {
		return this.attributes.get(name);
	}
	focus() { this.focused = true; }
	setPointerCapture() {
		if (this.captureError)
			throw new Error('pointer capture unavailable');
	}
	closest() { return null; }
	replaceChildren(...children) { this.options = children; }
	add(option) { this.options.push(option); }
	getContext(_kind, options = {}) {
		const isFpsOsd = this.id === 'fps-osd';
		if (isFpsOsd)
			fpsOsdAlpha = options.alpha;
		return {
			imageSmoothingEnabled: false,
			fillStyle: '#000',
			clearRect(_x, _y, _width, _height) {
				if (!isFpsOsd)
					return;
				fpsOsdCurrentRects = [];
			},
			fillRect(x, y, width, height) {
				if (!isFpsOsd) {
					fillRectCount++;
					return;
				}
				const rect = {
					kind: 'fill', color: this.fillStyle, x, y, width, height
				};
				fpsOsdCurrentRects.push(rect);
			},
			createImageData(width, height) {
				return { data: new Uint8ClampedArray(width * height * 4) };
			},
			putImageData(image) {
				putImageDataCount++;
				lastRawPixel = Array.from(image.data.slice(0, 4));
			},
			drawImage(_bitmap, _x, _y, width) {
				drawImageCount++;
				drawnWidths.push(width);
			}
		};
	}
}

const elements = new Map();
for (const id of [
	'screen', 'fps-osd', 'status', 'rom-list', 'btn-start', 'btn-pause', 'btn-reset',
	'btn-mute', 'btn-refresh', 'btn-load', 'upload', 'btn-controls',
	'btn-controls-close', 'btn-controls-reset', 'controls-panel',
	'controls-message', 'state-slot', 'state-label', 'state-message',
	'btn-state-save', 'btn-state-load', 'btn-state-delete',
	'btn-state-refresh', 'game-stage', 'screen-frame', 'display-mode',
	'btn-fullscreen', 'display-message', 'touch-controls'
])
	elements.set(id, new MockElement(id));
elements.get('controls-panel').hidden = true;
elements.get('fps-osd').hidden = true;
elements.get('game-stage').clientWidth = 390;
elements.get('game-stage').clientHeight = 844;
elements.get('game-stage').dataset.touchControls = 'visible';
elements.get('game-stage').computedStyle = {
	paddingLeft: '20px',
	paddingRight: '10px',
	paddingTop: '47px',
	paddingBottom: '34px'
};

const controlNames = [
	'up', 'down', 'left', 'right', 'b', 'a', 'select', 'start'
];
const touchControls = [];
for (const name of controlNames) {
	const touch = new MockElement(`touch-${name}`);
	touch.dataset.control = name;
	touchControls.push(touch);
	for (let slot = 0; slot < 2; slot++) {
		const binding = new MockElement(`binding-${name}-${slot}`);
		binding.dataset.bindingControl = name;
		binding.dataset.bindingSlot = String(slot);
		elements.set(binding.id, binding);
	}
}

const documentListeners = new Map();
let documentFocused = false;
let fullscreenRequestCount = 0;
let fullscreenExitCount = 0;
let fullscreenRequestError = null;
let fullscreenRequestHangs = false;
const documentMock = {
	hidden: false,
	fullscreenElement: null,
	fullscreenEnabled: true,
	hasFocus() { return documentFocused; },
	getElementById(id) {
		if (!elements.has(id))
			elements.set(id, new MockElement(id));
		return elements.get(id);
	},
	querySelectorAll(selector) {
		if (selector === '[data-control]')
			return touchControls;
		if (selector === '[data-binding-control]')
			return Array.from(elements.values()).filter(
				element => element.dataset.bindingControl
			);
		if (selector === 'button.down')
			return touchControls.filter(
				element => element.classList.contains('down')
			);
		return Array.from(elements.values());
	},
	addEventListener(name, callback) {
		documentListeners.set(name, callback);
	},
	exitFullscreen() {
		fullscreenExitCount++;
		this.fullscreenElement = null;
		const callback = documentListeners.get('fullscreenchange');
		if (callback)
			callback({ type: 'fullscreenchange' });
		return Promise.resolve();
	}
};
elements.get('game-stage').requestFullscreen = () => {
	fullscreenRequestCount++;
	if (fullscreenRequestError)
		return Promise.reject(fullscreenRequestError);
	if (fullscreenRequestHangs)
		return new Promise(() => {});
	documentMock.fullscreenElement = elements.get('game-stage');
	const callback = documentListeners.get('fullscreenchange');
	if (callback)
		callback({ type: 'fullscreenchange' });
	return Promise.resolve();
};

const windowListeners = new Map();
const windowMock = {
	addEventListener(name, callback) {
		windowListeners.set(name, callback);
	},
	getComputedStyle(element) { return element.computedStyle; },
	confirm() { return true; }
};
let gamepads = [];
const navigatorMock = {
	onLine: true,
	getGamepads() { return gamepads; }
};

let nextAnimationFrameId = 1;
const animationFrames = new Map();
function requestAnimationFrameMock(callback) {
	const id = nextAnimationFrameId++;
	animationFrames.set(id, callback);
	return id;
}
function cancelAnimationFrameMock(id) {
	animationFrames.delete(id);
}
function runAnimationFrameCycle() {
	const pending = Array.from(animationFrames.entries());
	for (const [id] of pending)
		animationFrames.delete(id);
	for (const [, callback] of pending)
		callback();
}

let monotonicMilliseconds = 0;
let nextIntervalId = 1;
const intervals = new Map();
const performanceMock = {
	now() { return monotonicMilliseconds; }
};
function setIntervalMock(callback, delay) {
	if (delay !== 1000)
		throw new Error(`unexpected FPS sampling interval: ${delay}`);
	const id = nextIntervalId++;
	intervals.set(id, callback);
	return id;
}
function clearIntervalMock(id) {
	intervals.delete(id);
}
function advanceFpsSample(milliseconds) {
	monotonicMilliseconds += milliseconds;
	for (const callback of Array.from(intervals.values()))
		callback();
}

let timeoutClock = 0;
let nextTimeoutId = 1000;
const timeouts = new Map();
function setTimeoutMock(callback, delay = 0) {
	const id = nextTimeoutId++;
	timeouts.set(id, {
		callback,
		at: timeoutClock + Math.max(0, Number(delay) || 0),
		delay: Math.max(0, Number(delay) || 0)
	});
	return id;
}
function clearTimeoutMock(id) {
	timeouts.delete(id);
}
function advanceTimeouts(milliseconds) {
	const target = timeoutClock + milliseconds;
	let callbacks = 0;
	for (;;) {
		let selectedId = null;
		let selected = null;
		for (const [id, timer] of timeouts) {
			if (timer.at <= target && (!selected || timer.at < selected.at ||
			    (timer.at === selected.at && id < selectedId))) {
				selectedId = id;
				selected = timer;
			}
		}
		if (!selected)
			break;
		if (++callbacks > 1000)
			throw new Error('mock timer loop did not quiesce');
		timeouts.delete(selectedId);
		monotonicMilliseconds += selected.at - timeoutClock;
		timeoutClock = selected.at;
		selected.callback();
	}
	monotonicMilliseconds += target - timeoutClock;
	timeoutClock = target;
}

const localStored = new Map([
	[
		'openwrt-nes-key-bindings-v1',
		'{"version":1,"bindings":{"up":["KeyQ","KeyQ"]}}'
	],
	[
		'openwrt-nes-display-mode-v1',
		'{"version":1,"mode":"16-9"}'
	]
]);
const sessionStorageMock = {
	setItem() { throw new Error('session storage blocked'); },
	getItem() { throw new Error('session storage blocked'); }
};
windowMock.localStorage = {
	setItem(key, value) { localStored.set(key, String(value)); },
	getItem(key) { return localStored.has(key) ? localStored.get(key) : null; }
};

class OptionMock {
	constructor(text, value) {
		this.text = text;
		this.value = value;
	}
}

let pendingLoadResolve;
let pendingUploadResolve;
let deferLoad = true;
let romLoaded = false;
let mockedState = null;
let failStateLoad = false;
let deferStateLoad = false;
let pendingStateLoadResolve;
let websocketCount = 0;
let lastWebSocket = null;
let failNextWebSocketConstruction = false;
let displayPreferenceRequests = 0;
let displayPreferenceFailure = null;
let deferDisplayPreference = false;
let pendingDisplayPreferenceResolve = null;
let displayPreferenceStatus = {
	t: 'status',
	show_fps: false,
	show_touch_controls: false
};
const jpegResolvers = [];
let jpegBitmapCloseCount = 0;
class WebSocketMock {
	static CONNECTING = 0;
	static OPEN = 1;
	static CLOSED = 3;

	constructor(url) {
		if (failNextWebSocketConstruction) {
			failNextWebSocketConstruction = false;
			throw new Error('mock WebSocket constructor failure');
		}
		websocketCount++;
		this.readyState = WebSocketMock.CONNECTING;
		this.sent = [];
		this.closeCount = 0;
		this.throwOnSend = false;
		this.bufferedAmount = 0;
		this.url = String(url);
		lastWebSocket = this;
	}

	close() {
		this.closeCount++;
		this.readyState = WebSocketMock.CLOSED;
	}
	send(message) {
		if (this.throwOnSend) {
			this.throwOnSend = false;
			throw new Error('mock send failure');
		}
		this.sent.push(message);
	}
}

let lastAudioContext = null;
const audioSources = [];
let deferAudioSuspend = false;
let pendingAudioSuspendResolve = null;
let deferAudioResume = false;
let pendingAudioResumeResolve = null;
class AudioContextMock {
	constructor() {
		this.state = 'running';
		this.currentTime = 0;
		this.destination = {};
		this.resumeCount = 0;
		this.suspendCount = 0;
		this.closeCount = 0;
		lastAudioContext = this;
	}
	createGain() {
		return { gain: { value: 1 }, connect() {} };
	}
	createBuffer(channels, frames, rate) {
		const data = Array.from(
			{ length: channels },
			() => new Float32Array(frames)
		);
		return {
			duration: frames / rate,
			getChannelData(channel) { return data[channel]; }
		};
	}
	createBufferSource() {
		const source = {
			buffer: null,
			stopped: false,
			startedAt: null,
			connect() {},
			start(at) { this.startedAt = at; },
			stop() {
				this.stopped = true;
				if (this.onended)
					this.onended();
			}
		};
		audioSources.push(source);
		return source;
	}
	resume() {
		this.resumeCount++;
		if (deferAudioResume) {
			return new Promise(resolve => {
				pendingAudioResumeResolve = () => {
					this.state = 'running';
					resolve();
				};
			});
		}
		this.state = 'running';
		return Promise.resolve();
	}
	suspend() {
		this.suspendCount++;
		if (deferAudioSuspend) {
			return new Promise(resolve => {
				pendingAudioSuspendResolve = () => {
					this.state = 'suspended';
					resolve();
				};
			});
		}
		this.state = 'suspended';
		return Promise.resolve();
	}
	close() {
		this.closeCount++;
		this.state = 'closed';
		return Promise.resolve();
	}
}
windowMock.AudioContext = AudioContextMock;

function response(body, status = 200) {
	return {
		ok: status >= 200 && status < 300,
		status,
		async json() { return body; },
		async text() { return JSON.stringify(body); }
	};
}

function rawPacket(rgb565) {
	const packet = new ArrayBuffer(14);
	const bytes = new Uint8Array(packet);
	const view = new DataView(packet);
	bytes[0] = 1;
	view.setUint16(2, 1, true);
	view.setUint16(4, 1, true);
	view.setUint16(12, rgb565, true);
	return packet;
}

function audioPacket(frames = 640, rate = 8000) {
	const packet = new ArrayBuffer(12 + frames * 2);
	const bytes = new Uint8Array(packet);
	const view = new DataView(packet);
	bytes[0] = 2;
	bytes[1] = 1;
	view.setUint32(4, rate, true);
	view.setUint32(8, frames, true);
	return packet;
}

async function fetchMock(resource, options = {}) {
	if (resource === '/api/status') {
		displayPreferenceRequests++;
		if (options.method !== 'GET' ||
		    options.headers.Authorization !==
		    'Bearer 0123456789abcdef0123456789abcdef') {
			throw new Error('initial display-preference probe was not authenticated GET');
		}
		if (displayPreferenceFailure)
			throw displayPreferenceFailure;
		if (deferDisplayPreference) {
			const deferredStatus = displayPreferenceStatus;
			return new Promise(resolve => {
				pendingDisplayPreferenceResolve = () =>
					resolve(response(deferredStatus));
			});
		}
		return response(displayPreferenceStatus);
	}
	if (resource === '/api/roms') {
		return response({
			roms: [{
				name: 'Super Mario Bros.nes',
				path: '/etc/nes-emulator/roms/Super Mario Bros.nes',
				readable: true
			}, {
				name: 'locked.nes',
				path: '/etc/nes-emulator/roms/locked.nes',
				readable: false,
				error: 'set group nesd and mode 0640'
			}]
		});
	}
	if (resource === '/api/states') {
		return response({
			ok: true,
			game_loaded: romLoaded,
			rom: romLoaded ? 'Super Mario Bros.nes' : '',
			rom_path: romLoaded
				? '/etc/nes-emulator/roms/Super Mario Bros.nes' : '',
			slots: Array.from({ length: 10 }, (_, index) => {
				const slot = index + 1;
				if (!mockedState || mockedState.slot !== slot)
					return { slot, exists: false, loadable: false,
						modified: 0, size: 0, label: '' };
				return { ...mockedState };
			})
		});
	}
	if (resource === '/api/load') {
		if (!deferLoad) {
			romLoaded = true;
			return response({ ok: true });
		}
		return new Promise(resolve => {
			pendingLoadResolve = () => {
				romLoaded = true;
				resolve(response({ ok: true }));
			};
		});
	}
	if (resource === '/api/state/save') {
		const body = JSON.parse(options.body || '{}');
		mockedState = {
			slot: body.slot,
			exists: true,
			loadable: true,
			modified: 1785600000,
			size: 14000,
			label: body.label || ''
		};
		return response({ ok: true, slot: body.slot, durable: true });
	}
	if (resource === '/api/state/load') {
		if (failStateLoad)
			return response({ error: 'Corrupt save state' }, 422);
		if (deferStateLoad) {
			return new Promise(resolve => {
				pendingStateLoadResolve = () => resolve(response({ ok: true }));
			});
		}
		return response({ ok: true });
	}
	if (resource === '/api/state/delete') {
		mockedState = null;
		return response({ ok: true, durable: true });
	}
	if (resource.startsWith('/api/upload?')) {
		return new Promise(resolve => {
			pendingUploadResolve = () => resolve(response({
				ok: true,
				name: 'Super Mario Bros.nes',
				path: '/etc/nes-emulator/roms/Super Mario Bros.nes'
			}));
		});
	}
	throw new Error(`unexpected fetch: ${resource}`);
}

const runClient = new Function(
	'document', 'window', 'location', 'sessionStorage', 'history',
	'navigator', 'Element', 'Option', 'WebSocket', 'fetch',
	'requestAnimationFrame', 'cancelAnimationFrame', 'createImageBitmap',
	'performance', 'setInterval', 'clearInterval', 'setTimeout', 'clearTimeout',
	scripts[0] + '\nreturn {' +
		' bootstrapAuthToken,' +
		' loadDisplayMode,' +
		' normalizeStoredDisplayMode,' +
		' fitDisplayBox,' +
		' uploadTimeoutMs,' +
		' refreshDisplayPreferences,' +
		' formatFpsValue,' +
		' syncCanvasDimensions,' +
		' getFpsOsdState: () => ({ value: fpsOsdValue,' +
			' generation: fpsSampleGeneration, timer: fpsSampleTimer }),' +
		' getDisplayModeLoadWarning: () => displayModeLoadWarning,' +
		' getHeartbeatState: () => ({ sequence: heartbeatSequence,' +
			' accepted: lastAcceptedHeartbeatSeq,' +
			' pending: Array.from(pendingHeartbeats.keys()) })' +
	'};'
);

let replacedHistoryUrl = null;
const locationMock = {
	search: '?token=query-token-must-not-bootstrap&view=compact',
	hash: '#token=0123456789abcdef0123456789abcdef',
	pathname: '/play',
	href: 'http://192.168.1.1:9090/play',
	protocol: 'http:'
};
const clientHooks = runClient(
	documentMock,
	windowMock,
	locationMock,
	sessionStorageMock,
	{ replaceState(_state, _title, url) { replacedHistoryUrl = url; } },
	navigatorMock,
	MockElement,
	OptionMock,
	WebSocketMock,
	fetchMock,
	requestAnimationFrameMock,
	cancelAnimationFrameMock,
	() => new Promise(resolve => {
		jpegResolvers.push(() => resolve({
			close() { jpegBitmapCloseCount++; }
		}));
	}),
	performanceMock,
	setIntervalMock,
	clearIntervalMock,
	setTimeoutMock,
	clearTimeoutMock
);

if (fpsOsdAlpha !== true ||
    elements.get('fps-osd').width !== elements.get('screen').width ||
    elements.get('fps-osd').height !== elements.get('screen').height) {
	throw new Error('FPS OSD dimensions did not follow the video canvas');
}
if (clientHooks.formatFpsValue(59.94) !== '59.9' ||
    clientHooks.formatFpsValue(0) !== '0.0' ||
    clientHooks.formatFpsValue(-1) !== '' ||
    clientHooks.formatFpsValue(Number.NaN) !== '') {
	throw new Error('FPS OSD rendered a label instead of only the numeric value');
}

if (clientHooks.uploadTimeoutMs(16) !== 40002 ||
    clientHooks.uploadTimeoutMs(8 * 1024) !== 41000 ||
    clientHooks.uploadTimeoutMs(16 * 1024 * 1024) !== 1810000 ||
    clientHooks.uploadTimeoutMs(Number.NaN) !== 40000) {
	throw new Error('size-derived upload timeout does not match the weak-link policy');
}

function requireDisplayFit(width, height, mode, expectedWidth, expectedHeight, label) {
	const fitted = clientHooks.fitDisplayBox(width, height, mode);
	const ratio = mode === '16-9' ? 16 / 9 : 4 / 3;
	const epsilon = 1e-9;
	if (Math.abs(fitted.width - expectedWidth) > epsilon ||
	    Math.abs(fitted.height - expectedHeight) > epsilon ||
	    fitted.width > width + epsilon || fitted.height > height + epsilon ||
	    Math.abs(fitted.width / fitted.height - ratio) > epsilon) {
		throw new Error(label);
	}
}
requireDisplayFit(390, 844, '4-3', 390, 292.5,
	'portrait 4:3 fullscreen fit distorted or escaped its content box');
requireDisplayFit(390, 844, '16-9', 390, 219.375,
	'portrait 16:9 fullscreen fit distorted or escaped its content box');
requireDisplayFit(844, 390, '4-3', 520, 390,
	'landscape 4:3 fullscreen fit distorted or escaped its content box');
requireDisplayFit(1936, 1064, '16-9', 1064 * 16 / 9, 1064,
	'wide 16:9 fullscreen fit distorted or escaped its content box');
const emptyDisplayFit = clientHooks.fitDisplayBox(Number.NaN, -1, '16-9');
if (emptyDisplayFit.width !== 0 || emptyDisplayFit.height !== 0)
	throw new Error('invalid fullscreen geometry did not fail closed');

function requirePseudoClassFreeFullscreenBounds(label) {
	const stage = elements.get('game-stage');
	const frame = elements.get('screen-frame');
	if (!stage.classList.contains('fullscreen-active'))
		throw new Error(`${label}: runtime fullscreen class is missing`);
	if (elements.get('fps-osd').width !== elements.get('screen').width ||
	    elements.get('fps-osd').height !== elements.get('screen').height) {
		throw new Error('FPS OSD dimensions did not follow the video canvas');
	}
	const pixels = value => {
		const parsed = Number.parseFloat(value);
		return Number.isFinite(parsed) ? parsed : 0;
	};
	const contentTop = pixels(stage.computedStyle.paddingTop);
	const contentBottom = stage.clientHeight -
		pixels(stage.computedStyle.paddingBottom);
	const contentHeight = Math.max(0, contentBottom - contentTop);
	const frameHeight = pixels(frame.style.getPropertyValue('height'));
	/* Deliberately ignore :fullscreen and :-webkit-full-screen. This models
	 * a browser that accepts the API but discards those CSS selectors. */
	const toolbarInFlow = !/position\s*:\s*absolute\s*;/
		.test(fullscreenToolbarDeclarations);
	const touchControlsInFlow = !/position\s*:\s*absolute\s*;/
		.test(fullscreenTouchDeclarations);
	const flowHeight = frameHeight + (toolbarInFlow ? 48 : 0) +
		(touchControlsInFlow ? 176 : 0);
	const frameTop = contentTop + (contentHeight - flowHeight) / 2;
	const frameBottom = frameTop + frameHeight;
	const epsilon = 1e-9;
	if (toolbarInFlow || touchControlsInFlow ||
	    frameTop < contentTop - epsilon ||
	    frameBottom > contentBottom + epsilon) {
		throw new Error(
			`${label}: class-only fullscreen layout clips the picture vertically`
		);
	}
}

function keyboardEvent(code) {
	return {
		code,
		repeat: false,
		target: new MockElement('keyboard-target'),
		preventDefault() {},
		stopPropagation() {}
	};
}

function heartbeatMessages(socket = lastWebSocket) {
	return socket.sent
		.map(message => JSON.parse(message))
		.filter(message => message.t === 'heartbeat');
}

function latestHeartbeat(socket = lastWebSocket) {
	const messages = heartbeatMessages(socket);
	if (!messages.length)
		throw new Error('no heartbeat is available to acknowledge');
	return messages.at(-1);
}

function acknowledgeHeartbeat(socket = lastWebSocket, seq = undefined) {
	const acknowledged = seq === undefined ? latestHeartbeat(socket).seq : seq;
	socket.onmessage({
		data: JSON.stringify({ t: 'heartbeat', seq: acknowledged })
	});
	return acknowledged;
}

const repairedBindings = JSON.parse(
	localStored.get('openwrt-nes-key-bindings-v1')
).bindings;
if (repairedBindings.up[0] !== 'ArrowUp' ||
    repairedBindings.up[1] !== 'KeyW' ||
    replacedHistoryUrl !== '/play?view=compact') {
	throw new Error('storage fallback did not repair controls or scrub the token URL');
}

const recoveredSessionToken = 'fedcba9876543210fedcba9876543210';
const sessionValues = new Map([
	['nes-auth-token', recoveredSessionToken]
]);
sessionStorageMock.setItem = (key, value) => {
	sessionValues.set(String(key), String(value));
};
sessionStorageMock.getItem = key => sessionValues.get(String(key)) || null;
locationMock.search = '?token=query-token-must-be-ignored&view=session';
locationMock.hash = '#controls=open';
if (clientHooks.bootstrapAuthToken() !== recoveredSessionToken ||
    replacedHistoryUrl !== '/play?view=session#controls=open' ||
    sessionValues.get('nes-auth-token') !== recoveredSessionToken) {
	throw new Error('query token bootstrapped the client instead of session storage');
}
sessionValues.delete('nes-auth-token');
locationMock.search = '?token=query-token-must-be-rejected&view=direct';
locationMock.hash = '#controls=open';
if (clientHooks.bootstrapAuthToken() !== '' ||
    replacedHistoryUrl !== '/play?view=direct#controls=open' ||
    sessionValues.has('nes-auth-token')) {
	throw new Error('query-only token bootstrapped an unauthenticated client');
}
locationMock.search = '?view=fragment';
locationMock.hash = '#token=abcdef0123456789abcdef0123456789&controls=open';
if (clientHooks.bootstrapAuthToken() !== 'abcdef0123456789abcdef0123456789' ||
    replacedHistoryUrl !== '/play?view=fragment#controls=open' ||
    sessionValues.get('nes-auth-token') !== 'abcdef0123456789abcdef0123456789') {
	throw new Error('fragment token was not retained, stored and scrubbed');
}
/* Runtime authentication remains bound to the token from initial bootstrap. */
locationMock.search = '';
locationMock.hash = '';
if (elements.get('binding-up-0').textContent !== '↑ Arrow' ||
    elements.get('binding-up-1').textContent !== 'W' ||
    elements.get('binding-b-0').textContent !== 'Z') {
	throw new Error('invalid saved controls did not fall back atomically to defaults');
}
elements.get('btn-controls').onclick();
if (elements.get('controls-panel').hidden ||
    !elements.get('controls-message').textContent.includes('invalid')) {
	throw new Error('controls panel did not report invalid stored bindings');
}

elements.get('binding-b-0').dispatch('click');
if (elements.get('binding-b-0').textContent !== 'Press a key…')
	throw new Error('binding capture did not start');
windowListeners.get('keydown')(keyboardEvent('KeyQ'));
let savedBindings = JSON.parse(
	localStored.get('openwrt-nes-key-bindings-v1')
).bindings;
if (savedBindings.b[0] !== 'KeyQ' ||
    elements.get('binding-b-0').textContent !== 'Q') {
	throw new Error('captured keyboard binding was not applied and saved');
}

elements.get('binding-a-0').dispatch('click');
windowListeners.get('keydown')(keyboardEvent('KeyQ'));
savedBindings = JSON.parse(
	localStored.get('openwrt-nes-key-bindings-v1')
).bindings;
if (savedBindings.a[0] !== 'KeyX' ||
    !elements.get('controls-message').textContent.includes('already assigned to B')) {
	throw new Error('duplicate keyboard binding was accepted silently');
}
windowListeners.get('keydown')(keyboardEvent('Escape'));

elements.get('binding-start-0').dispatch('click');
windowListeners.get('keydown')(keyboardEvent('Delete'));
savedBindings = JSON.parse(
	localStored.get('openwrt-nes-key-bindings-v1')
).bindings;
if (savedBindings.start[0] !== null ||
    elements.get('binding-start-0').textContent !== 'Unbound') {
	throw new Error('keyboard binding could not be cleared');
}

elements.get('btn-controls-reset').onclick();
savedBindings = JSON.parse(
	localStored.get('openwrt-nes-key-bindings-v1')
).bindings;
if (savedBindings.b[0] !== 'KeyZ' ||
    savedBindings.start[0] !== 'Enter' ||
    elements.get('binding-b-0').textContent !== 'Z') {
	throw new Error('Restore defaults did not restore and persist the keymap');
}

if (elements.get('screen-frame').dataset.displayMode !== '16-9' ||
    elements.get('display-mode').value !== '16-9') {
	throw new Error('valid stored 16:9 picture mode was not restored');
}
localStored.set('openwrt-nes-display-mode-v1', '{broken');
if (clientHooks.loadDisplayMode() !== '4-3' ||
    JSON.parse(localStored.get('openwrt-nes-display-mode-v1')).mode !== '4-3' ||
    !clientHooks.getDisplayModeLoadWarning().includes('invalid')) {
	throw new Error('malformed stored picture mode was not repaired to 4:3');
}
localStored.set(
	'openwrt-nes-display-mode-v1',
	'{"version":2,"mode":"16-9"}'
);
const unknownSchemaMode = clientHooks.loadDisplayMode();
const repairedUnknownSchema = JSON.parse(
	localStored.get('openwrt-nes-display-mode-v1')
);
if (unknownSchemaMode !== '4-3' || repairedUnknownSchema.version !== 1 ||
    repairedUnknownSchema.mode !== '4-3' ||
    !clientHooks.getDisplayModeLoadWarning().includes('invalid')) {
	throw new Error('unknown stored picture-mode schema was accepted');
}
const originalLocalStorageGetItem = windowMock.localStorage.getItem;
windowMock.localStorage.getItem = () => {
	throw new Error('local storage read blocked');
};
if (clientHooks.loadDisplayMode() !== '4-3' ||
    !clientHooks.getDisplayModeLoadWarning().includes('could not be read')) {
	throw new Error('picture-mode storage read failure did not use 4:3 safely');
}
windowMock.localStorage.getItem = originalLocalStorageGetItem;
localStored.set(
	'openwrt-nes-display-mode-v1',
	'{"version":1,"mode":"16-9"}'
);
const backingWidthBeforeAspectChange = elements.get('screen').width;
const backingHeightBeforeAspectChange = elements.get('screen').height;
elements.get('display-mode').value = '4-3';
elements.get('display-mode').onchange();
const storedDisplayMode = JSON.parse(
	localStored.get('openwrt-nes-display-mode-v1')
);
if (elements.get('screen-frame').dataset.displayMode !== '4-3' ||
	    storedDisplayMode.version !== 1 || storedDisplayMode.mode !== '4-3' ||
	    elements.get('screen').width !== backingWidthBeforeAspectChange ||
	    elements.get('screen').height !== backingHeightBeforeAspectChange ||
	    elements.get('fps-osd').width !== backingWidthBeforeAspectChange ||
	    elements.get('fps-osd').height !== backingHeightBeforeAspectChange ||
	    websocketCount !== 0) {
	throw new Error('4:3 picture selection changed rendering state or was not saved');
}
const originalLocalStorageSetItem = windowMock.localStorage.setItem;
windowMock.localStorage.setItem = () => {
	throw new Error('local storage blocked');
};
elements.get('display-mode').value = '16-9';
elements.get('display-mode').onchange();
if (elements.get('screen-frame').dataset.displayMode !== '16-9' ||
    !elements.get('display-message').textContent.includes('active for this tab')) {
	throw new Error('blocked picture-mode storage prevented the live selection');
}
windowMock.localStorage.setItem = originalLocalStorageSetItem;
elements.get('display-mode').value = '4-3';
elements.get('display-mode').onchange();

(async () => {
	await new Promise(resolve => setImmediate(resolve));
	if (displayPreferenceRequests !== 1 ||
		    !elements.get('touch-controls').hidden ||
		    !elements.get('fps-osd').hidden || fpsOsdCurrentRects.length !== 0 ||
		    websocketCount !== 0) {
		throw new Error(
			'initial authenticated status probe did not hide configured client overlays before Start'
		);
	}
	elements.get('touch-controls').hidden = false;
	displayPreferenceStatus = { t: 'status', show_fps: false };
	if (!await clientHooks.refreshDisplayPreferences() ||
	    elements.get('touch-controls').hidden) {
		throw new Error('missing touch-control preference did not fail open');
	}
	displayPreferenceStatus = {
		t: 'status', show_fps: false, show_touch_controls: 'false'
	};
	if (!await clientHooks.refreshDisplayPreferences() ||
	    elements.get('touch-controls').hidden) {
		throw new Error('non-boolean touch-control preference did not fail open');
	}
	displayPreferenceFailure = new Error('router temporarily unavailable');
	if (await clientHooks.refreshDisplayPreferences() ||
	    elements.get('touch-controls').hidden) {
		throw new Error('failed initial preference request did not fail open');
	}
	displayPreferenceFailure = null;
	displayPreferenceStatus = {
		t: 'status', show_fps: false, show_touch_controls: false
	};
	if (!await clientHooks.refreshDisplayPreferences() ||
	    !elements.get('touch-controls').hidden || websocketCount !== 0) {
		throw new Error('touch controls were not hidden again before stream startup');
	}
	const connectionStatusBeforeFullscreen = elements.get('status').textContent;
	if (!await elements.get('btn-fullscreen').onclick() ||
	    documentMock.fullscreenElement !== elements.get('game-stage') ||
	    fullscreenRequestCount !== 1 ||
	    elements.get('btn-fullscreen').textContent !== 'Exit fullscreen' ||
	    elements.get('btn-fullscreen').getAttribute('aria-pressed') !== 'true' ||
	    !elements.get('game-stage').classList.contains('fullscreen-active') ||
	    elements.get('screen-frame').dataset.displayMode !== '4-3' ||
	    elements.get('screen-frame').style.getPropertyValue('width') !== '360px' ||
	    elements.get('screen-frame').style.getPropertyValue('height') !== '270px' ||
	    elements.get('screen-frame').style.getPropertyValue('max-width') !== '360px' ||
	    elements.get('screen-frame').style.getPropertyValue('max-height') !== '270px') {
		throw new Error('fullscreen entry did not preserve display mode or button state');
	}
	elements.get('game-stage').clientWidth = 1920;
	elements.get('game-stage').clientHeight = 1080;
	elements.get('game-stage').computedStyle = {
		paddingLeft: '0px', paddingRight: '0px',
		paddingTop: '0px', paddingBottom: '0px'
	};
	windowListeners.get('resize')();
	if (elements.get('screen-frame').style.getPropertyValue('width') !== '1440px' ||
	    elements.get('screen-frame').style.getPropertyValue('height') !== '1080px') {
		throw new Error('full-height 4:3 picture was not fitted to a 1920x1080 viewport');
	}
	requirePseudoClassFreeFullscreenBounds('1920x1080 4:3 regression');
	elements.get('game-stage').clientWidth = 844;
	elements.get('game-stage').clientHeight = 390;
	elements.get('game-stage').computedStyle = {
		paddingLeft: '0px', paddingRight: '0px',
		paddingTop: '0px', paddingBottom: '0px'
	};
	windowListeners.get('resize')();
	if (elements.get('screen-frame').style.getPropertyValue('width') !== '520px' ||
	    elements.get('screen-frame').style.getPropertyValue('height') !== '390px') {
		throw new Error('fullscreen resize did not refit the active 4:3 picture');
	}
	elements.get('game-stage').clientWidth = 390;
	elements.get('game-stage').clientHeight = 844;
	windowListeners.get('orientationchange')();
	runAnimationFrameCycle();
	if (elements.get('screen-frame').style.getPropertyValue('width') !== '390px' ||
	    elements.get('screen-frame').style.getPropertyValue('height') !== '292.5px') {
		throw new Error('fullscreen orientation change retained stale geometry');
	}
	elements.get('display-mode').value = '16-9';
	elements.get('display-mode').onchange();
	if (elements.get('screen-frame').style.getPropertyValue('width') !== '390px' ||
	    elements.get('screen-frame').style.getPropertyValue('height') !== '219.375px') {
		throw new Error('live 16:9 fullscreen selection retained 4:3 geometry');
	}
	elements.get('display-mode').value = '4-3';
	elements.get('display-mode').onchange();
	if (!await elements.get('btn-fullscreen').onclick() ||
	    documentMock.fullscreenElement !== null || fullscreenExitCount !== 1 ||
	    elements.get('btn-fullscreen').textContent !== 'Fullscreen' ||
	    elements.get('btn-fullscreen').getAttribute('aria-pressed') !== 'false' ||
	    elements.get('game-stage').classList.contains('fullscreen-active') ||
	    elements.get('screen-frame').style.getPropertyValue('width') !== '' ||
	    elements.get('screen-frame').style.getPropertyValue('height') !== '') {
		throw new Error('fullscreen button could not leave fullscreen');
	}
	await elements.get('btn-fullscreen').onclick();
	documentMock.fullscreenElement = null;
	documentListeners.get('fullscreenchange')({ type: 'fullscreenchange' });
	if (elements.get('btn-fullscreen').textContent !== 'Fullscreen' ||
	    elements.get('btn-fullscreen').getAttribute('aria-pressed') !== 'false' ||
	    elements.get('game-stage').classList.contains('fullscreen-active')) {
		throw new Error('external fullscreen exit left stale button state');
	}
	fullscreenRequestError = new Error('permission denied');
	if (await elements.get('btn-fullscreen').onclick() ||
	    elements.get('btn-fullscreen').disabled ||
	    elements.get('game-stage').classList.contains('fullscreen-active') ||
	    !elements.get('display-message').textContent.includes('permission denied') ||
	    elements.get('status').textContent !== connectionStatusBeforeFullscreen) {
		throw new Error('fullscreen rejection corrupted UI or connection status');
	}
	documentListeners.get('fullscreenerror')({ type: 'fullscreenerror' });
	if (elements.get('game-stage').classList.contains('fullscreen-active') ||
	    !elements.get('display-message').textContent.includes(
		    'browser rejected the transition')) {
		throw new Error('fullscreenerror left stale runtime layout state');
	}
	fullscreenRequestError = null;
	fullscreenRequestHangs = true;
	const hungFullscreen = elements.get('btn-fullscreen').onclick();
	if (!elements.get('btn-fullscreen').disabled)
		throw new Error('pending fullscreen transition did not disable duplicate requests');
	const requestsDuringHungFullscreen = fullscreenRequestCount;
	if (await elements.get('btn-fullscreen').onclick() !== false ||
	    fullscreenRequestCount !== requestsDuringHungFullscreen) {
		throw new Error('fullscreen transition accepted a reentrant request');
	}
	advanceTimeouts(5000);
	if (await hungFullscreen || elements.get('btn-fullscreen').disabled ||
	    !elements.get('display-message').textContent.includes('in time')) {
		throw new Error('hung fullscreen promise did not recover after its deadline');
	}
	fullscreenRequestHangs = false;
	elements.get('display-mode').onchange();
	const standardRequestFullscreen =
		elements.get('game-stage').requestFullscreen;
	const standardExitFullscreen = documentMock.exitFullscreen;
	elements.get('game-stage').requestFullscreen = undefined;
	documentMock.exitFullscreen = undefined;
	documentMock.fullscreenEnabled = false;
	let webkitRequestCount = 0;
	let webkitExitCount = 0;
	elements.get('game-stage').webkitRequestFullscreen = () => {
		webkitRequestCount++;
		documentMock.webkitFullscreenElement = elements.get('game-stage');
		documentListeners.get('webkitfullscreenchange')({
			type: 'webkitfullscreenchange'
		});
	};
	documentMock.webkitExitFullscreen = () => {
		webkitExitCount++;
		documentMock.webkitFullscreenElement = null;
		documentListeners.get('webkitfullscreenchange')({
			type: 'webkitfullscreenchange'
		});
	};
	documentMock.webkitFullscreenEnabled = true;
	documentListeners.get('fullscreenchange')({ type: 'fullscreenchange' });
	if (!await elements.get('btn-fullscreen').onclick() ||
	    webkitRequestCount !== 1 ||
	    elements.get('btn-fullscreen').textContent !== 'Exit fullscreen' ||
	    !elements.get('game-stage').classList.contains('fullscreen-active')) {
		throw new Error('WebKit-prefixed fullscreen entry is not functional');
	}
	if (!await elements.get('btn-fullscreen').onclick() ||
	    webkitExitCount !== 1 ||
	    elements.get('game-stage').classList.contains('fullscreen-active')) {
		throw new Error('WebKit-prefixed fullscreen fallback is not functional');
	}
	elements.get('game-stage').webkitRequestFullscreen = undefined;
	documentMock.webkitExitFullscreen = undefined;
	documentMock.webkitFullscreenEnabled = false;
	documentListeners.get('fullscreenchange')({ type: 'fullscreenchange' });
	if (!elements.get('btn-fullscreen').disabled ||
	    elements.get('display-mode').disabled) {
		throw new Error('unsupported fullscreen disabled unrelated display controls');
	}
	elements.get('game-stage').requestFullscreen = standardRequestFullscreen;
	documentMock.exitFullscreen = standardExitFullscreen;
	documentMock.fullscreenEnabled = true;
	documentListeners.get('fullscreenchange')({ type: 'fullscreenchange' });
	if (elements.get('btn-fullscreen').disabled)
		throw new Error('fullscreen button did not recover after API became available');

	await new Promise(resolve => setImmediate(resolve));
	const lockedOption = elements.get('rom-list').options.find(
		option => option.value === '/etc/nes-emulator/roms/locked.nes'
	);
	if (!lockedOption || lockedOption.disabled !== true ||
	    !lockedOption.text.includes('not readable by nesd')) {
		throw new Error('unreadable ROM is hidden or selectable in game client');
	}
	elements.get('rom-list').value =
		'/etc/nes-emulator/roms/Super Mario Bros.nes';
	const loadAction = elements.get('btn-load').onclick();
	await new Promise(resolve => setImmediate(resolve));
	if (typeof pendingLoadResolve !== 'function')
		throw new Error('load action did not reach the API');

	documentMock.hidden = true;
	documentListeners.get('visibilitychange')();
	pendingLoadResolve();
	await loadAction;

	if (websocketCount !== 0) {
		throw new Error(
			'late load completion opened a WebSocket in a hidden tab'
		);
	}
	if (elements.get('status').textContent !==
	    'stream remains stopped while this tab is hidden') {
		throw new Error('late load completion did not reach the hidden-tab guard');
	}

	deferLoad = false;
	documentMock.hidden = false;
	const upload = elements.get('upload');
	upload.files = [{
		name: 'Super Mario Bros.nes',
		size: 16
	}];
	const uploadAction = upload.onchange({ target: upload });
	await new Promise(resolve => setImmediate(resolve));
	if (typeof pendingUploadResolve !== 'function')
		throw new Error('upload action did not reach the API');

	documentMock.hidden = true;
	documentListeners.get('visibilitychange')();
	pendingUploadResolve();
	await uploadAction;

	if (websocketCount !== 0) {
		throw new Error(
			'late upload completion opened a WebSocket in a hidden tab'
		);
	}
	if (elements.get('status').textContent !==
	    'stream remains stopped while this tab is hidden') {
		throw new Error(
			'late upload completion hid the stream-suppression warning'
		);
	}

	documentMock.hidden = false;
	documentListeners.get('visibilitychange')();
	if (websocketCount !== 1 || !lastWebSocket) {
		throw new Error('visible tab did not resume the stream requested while hidden');
	}
	if (lastWebSocket.url !==
	    'ws://192.168.1.1:9090/ws?token=0123456789abcdef0123456789abcdef') {
		throw new Error('WebSocket query authentication was not preserved');
	}
	documentListeners.get('visibilitychange')();
	if (websocketCount !== 1)
		throw new Error('visible-tab resume created duplicate WebSockets');
	elements.get('rom-list').value =
		'/etc/nes-emulator/roms/Super Mario Bros.nes';
	await elements.get('btn-load').onclick();
	if (websocketCount !== 1 || !lastWebSocket)
		throw new Error('successful load did not create one game WebSocket');
	lastWebSocket.readyState = WebSocketMock.OPEN;
	lastWebSocket.onopen();
	const openMessages = lastWebSocket.sent.map(message => JSON.parse(message));
	if (openMessages.length < 2 || openMessages[0].t !== 'hello' ||
	    openMessages[1].t !== 'input') {
		throw new Error('WebSocket open did not establish hello before controller input');
	}
	if (!elements.get('fps-osd').hidden || fpsOsdCurrentRects.length !== 0 ||
	    intervals.size !== 0) {
		throw new Error('FPS OSD started before receiving its server setting');
	}
	displayPreferenceStatus = {
		t: 'status', show_fps: false, show_touch_controls: false
	};
	deferDisplayPreference = true;
	const staleDisplayPreference = clientHooks.refreshDisplayPreferences();
	await new Promise(resolve => setImmediate(resolve));
	if (typeof pendingDisplayPreferenceResolve !== 'function')
		throw new Error('deferred initial display preference did not reach fetch');
	lastWebSocket.onmessage({
		data: JSON.stringify({
			t: 'status',
			game_loaded: true,
			rom_path: '/etc/nes-emulator/roms/Super Mario Bros.nes',
			show_fps: true,
			show_touch_controls: true,
			core: 'FCEUmm',
			rom: 'Super Mario Bros.nes',
			paused: false
		})
	});
	pendingDisplayPreferenceResolve();
	if (await staleDisplayPreference !== false)
		throw new Error('late HTTP display preference was applied after WebSocket status');
	deferDisplayPreference = false;
	pendingDisplayPreferenceResolve = null;
	if (elements.get('fps-osd').hidden || intervals.size !== 1 ||
	    clientHooks.getFpsOsdState().value !== '' ||
	    fpsOsdCurrentRects.length !== 0) {
		throw new Error('enabled FPS OSD did not start blank with exactly one sampler');
	}
	if (elements.get('touch-controls').hidden)
		throw new Error('enabled touch controls were hidden by server status');
	lastWebSocket.onmessage({
		data: JSON.stringify({ t: 'status', show_fps: true,
			game_loaded: true,
			rom_path: '/etc/nes-emulator/roms/Super Mario Bros.nes' })
	});
	if (intervals.size !== 1)
		throw new Error('repeated status created a duplicate FPS sampler');

	function latestInputMask() {
		const messages = lastWebSocket.sent
			.map(message => JSON.parse(message))
			.filter(message => message.t === 'input');
		if (!messages.length)
			throw new Error('no input message was sent');
		return messages[messages.length - 1].mask;
	}

	function inputMessageCount() {
		return lastWebSocket.sent
			.map(message => JSON.parse(message))
			.filter(message => message.t === 'input')
			.length;
	}

	windowListeners.get('keydown')(keyboardEvent('KeyZ'));
	const inputsBeforeFullscreen = inputMessageCount();
	if (latestInputMask() !== 1 ||
	    !await elements.get('btn-fullscreen').onclick() ||
	    latestInputMask() !== 0 ||
	    inputMessageCount() !== inputsBeforeFullscreen + 1) {
		throw new Error('fullscreen entry did not release held controller input once');
	}
	lastWebSocket.onmessage({
		data: JSON.stringify({ t: 'status', show_touch_controls: false,
			game_loaded: true,
			rom_path: '/etc/nes-emulator/roms/Super Mario Bros.nes' })
	});
	if (!elements.get('touch-controls').hidden ||
	    elements.get('game-stage').dataset.touchControls !== 'hidden' ||
	    !elements.get('game-stage').classList.contains('fullscreen-active')) {
		throw new Error('touch controls did not hide while fullscreen was active');
	}
	lastWebSocket.onmessage({
		data: JSON.stringify({ t: 'status', show_touch_controls: true,
			game_loaded: true,
			rom_path: '/etc/nes-emulator/roms/Super Mario Bros.nes' })
	});
	if (elements.get('touch-controls').hidden ||
	    elements.get('game-stage').dataset.touchControls !== 'visible')
		throw new Error('touch controls did not return while fullscreen was active');
	await elements.get('btn-fullscreen').onclick();

	await new Promise(resolve => setImmediate(resolve));
	if (elements.get('btn-state-save').disabled ||
	    !elements.get('btn-state-load').disabled ||
	    elements.get('state-slot').options.length !== 10) {
		throw new Error('empty save-state slots have incorrect action availability');
	}
	elements.get('state-label').value = 'Before castle';
	await elements.get('btn-state-save').onclick();
	if (!elements.get('state-slot').options[0].text.includes('Before castle') ||
	    elements.get('btn-state-load').disabled) {
		throw new Error('saved state was not rendered as a loadable named slot');
	}
	lastWebSocket.onmessage({
		data: JSON.stringify({ t: 'status', show_fps: true,
			game_loaded: true, paused: true,
			rom_path: '/etc/nes-emulator/roms/Super Mario Bros.nes' })
	});
	lastWebSocket.onmessage({ data: rawPacket(0x07e0) });
	runAnimationFrameCycle();
	const paintsBeforeFailedStateLoad = putImageDataCount;
	const fillsBeforeFailedStateLoad = fillRectCount;
	/* A frame received just before the rejection must not be discarded. */
	lastWebSocket.onmessage({ data: rawPacket(0x001f) });
	failStateLoad = true;
	await elements.get('btn-state-load').onclick();
	failStateLoad = false;
	if (fillRectCount !== fillsBeforeFailedStateLoad ||
	    putImageDataCount !== paintsBeforeFailedStateLoad ||
	    lastRawPixel.join(',') !== '0,255,0,255' ||
	    !elements.get('state-message').textContent.includes('Corrupt save state')) {
		throw new Error('failed paused save-state load erased the visible frame');
	}
	runAnimationFrameCycle();
	if (putImageDataCount !== paintsBeforeFailedStateLoad + 1 ||
	    lastRawPixel.join(',') !== '0,0,255,255') {
		throw new Error('failed save-state load discarded an already received frame');
	}

	windowListeners.get('keydown')(keyboardEvent('KeyZ'));
	if (latestInputMask() !== 1)
		throw new Error('save-state load setup did not hold the B button');
	deferStateLoad = true;
	const stateLoadAction = elements.get('btn-state-load').onclick();
	await new Promise(resolve => setImmediate(resolve));
	if (typeof pendingStateLoadResolve !== 'function')
		throw new Error('save-state load did not reach the deferred API');
	const fillsBeforeRestoredState = fillRectCount;
	const paintsBeforeRestoredState = putImageDataCount;
	lastWebSocket.onmessage({
		data: JSON.stringify({ t: 'status', event: 'state-loaded',
			show_fps: true, game_loaded: true, paused: true,
			rom_path: '/etc/nes-emulator/roms/Super Mario Bros.nes' })
	});
	lastWebSocket.onmessage({ data: rawPacket(0xf800) });
	runAnimationFrameCycle();
	if (fillRectCount !== fillsBeforeRestoredState + 1 ||
	    putImageDataCount !== paintsBeforeRestoredState + 1 ||
	    lastRawPixel.join(',') !== '255,0,0,255') {
		throw new Error('state-loaded event did not paint the restored frame');
	}
	const fillsAfterRestoredState = fillRectCount;
	const paintsAfterRestoredState = putImageDataCount;
	pendingStateLoadResolve();
	await stateLoadAction;
	deferStateLoad = false;
	pendingStateLoadResolve = null;
	if (latestInputMask() !== 0 ||
	    !elements.get('state-message').textContent.includes('loaded') ||
	    fillRectCount !== fillsAfterRestoredState ||
	    putImageDataCount !== paintsAfterRestoredState ||
	    lastRawPixel.join(',') !== '255,0,0,255') {
		throw new Error(
			'loading a state did not release input, report success, or preserve the restored frame'
		);
	}
	await elements.get('btn-state-delete').onclick();
	if (!elements.get('state-slot').options[0].text.includes('Empty') ||
	    !elements.get('btn-state-load').disabled) {
		throw new Error('deleted state remained loadable in the slot list');
	}

	for (const [code, expectedMask] of [
		['KeyZ', 1],
		['ShiftLeft', 4],
		['Enter', 8],
		['ArrowUp', 16],
		['ArrowDown', 32],
		['ArrowLeft', 64],
		['ArrowRight', 128],
		['KeyX', 256]
	]) {
		windowListeners.get('keydown')(keyboardEvent(code));
		if (latestInputMask() !== expectedMask)
			throw new Error(`${code} produced mask ${latestInputMask()}`);
		windowListeners.get('keyup')(keyboardEvent(code));
		if (latestInputMask() !== 0)
			throw new Error(`${code} remained held after keyup`);
	}

	const touchA = touchControls.find(control => control.dataset.control === 'a');
	const touchB = touchControls.find(control => control.dataset.control === 'b');
	const pointerEvent = pointerId => ({
		pointerId,
		preventDefault() {}
	});
	touchA.dispatch('pointerdown', pointerEvent(1));
	touchB.dispatch('pointerdown', pointerEvent(2));
	if (latestInputMask() !== 257)
		throw new Error('multitouch A+B did not produce the combined NES mask');
	touchA.dispatch('pointerup', pointerEvent(1));
	if (latestInputMask() !== 1)
		throw new Error('releasing A also released the held B button');
	touchB.dispatch('pointerup', pointerEvent(2));
	if (latestInputMask() !== 0)
		throw new Error('touch control remained held after pointerup');

	touchA.captureError = true;
	touchA.dispatch('pointerdown', pointerEvent(3));
	if (latestInputMask() !== 256)
		throw new Error('capture failure prevented the touch button from activating');
	windowListeners.get('pointerup')(pointerEvent(3));
	if (latestInputMask() !== 0)
		throw new Error('window-level pointerup did not recover failed pointer capture');
	touchA.captureError = false;

	windowListeners.get('keydown')(keyboardEvent('KeyZ'));
	touchA.dispatch('pointerdown', pointerEvent(4));
	if (latestInputMask() !== 257)
		throw new Error('touch-control visibility setup did not hold keyboard B and touch A');
	lastWebSocket.onmessage({
		data: JSON.stringify({ t: 'status', show_touch_controls: false,
			game_loaded: true,
			rom_path: '/etc/nes-emulator/roms/Super Mario Bros.nes' })
	});
	if (!elements.get('touch-controls').hidden ||
	    elements.get('game-stage').dataset.touchControls !== 'hidden' ||
	    latestInputMask() !== 1 ||
	    touchA.classList.contains('down') ||
	    touchA.getAttribute('aria-pressed') !== 'false') {
		throw new Error(
			'hiding touch controls did not release pointer input while preserving keyboard input'
		);
	}
	const inputsBeforeHiddenPointer = inputMessageCount();
	touchA.dispatch('pointerdown', pointerEvent(5));
	if (latestInputMask() !== 1 ||
	    inputMessageCount() !== inputsBeforeHiddenPointer ||
	    touchA.classList.contains('down')) {
		throw new Error('hidden touch control accepted a late pointerdown');
	}
	/* Emulate a queued pointerdown becoming active just before repeated status. */
	elements.get('touch-controls').hidden = false;
	touchA.dispatch('pointerdown', pointerEvent(6));
	if (latestInputMask() !== 257)
		throw new Error('repeated hidden-status race setup did not activate touch A');
	elements.get('touch-controls').hidden = true;
	lastWebSocket.onmessage({
		data: JSON.stringify({ t: 'status', show_touch_controls: false,
			game_loaded: true,
			rom_path: '/etc/nes-emulator/roms/Super Mario Bros.nes' })
	});
	if (latestInputMask() !== 1 || touchA.classList.contains('down')) {
		throw new Error('repeated hidden status did not clean up raced pointer input');
	}
	const inputsAfterRepeatedCleanup = inputMessageCount();
	lastWebSocket.onmessage({
		data: JSON.stringify({ t: 'status', show_touch_controls: false,
			game_loaded: true,
			rom_path: '/etc/nes-emulator/roms/Super Mario Bros.nes' })
	});
	if (inputMessageCount() !== inputsAfterRepeatedCleanup)
		throw new Error('idempotent hidden status emitted duplicate controller input');
	windowListeners.get('keyup')(keyboardEvent('KeyZ'));
	if (latestInputMask() !== 0)
		throw new Error('keyboard input remained held after hidden touch controls released');
	lastWebSocket.onmessage({
		data: JSON.stringify({ t: 'status', show_touch_controls: true,
			game_loaded: true,
			rom_path: '/etc/nes-emulator/roms/Super Mario Bros.nes' })
	});
	if (elements.get('touch-controls').hidden ||
	    elements.get('game-stage').dataset.touchControls !== 'visible')
		throw new Error('touch controls could not be restored by server status');

	const pad = {
		buttons: Array.from({ length: 16 }, () => ({ pressed: false })),
		axes: [0, 0]
	};
	pad.buttons[0].pressed = true;
	pad.buttons[12].pressed = true;
	gamepads = [pad];
	const inputsBeforeGamepad = inputMessageCount();
	runAnimationFrameCycle();
	if (inputMessageCount() !== inputsBeforeGamepad)
		throw new Error('gamepad input activated in an initially unfocused window');
	documentFocused = true;
	windowListeners.get('focus')();
	runAnimationFrameCycle();
	if (latestInputMask() !== 272 ||
	    inputMessageCount() !== inputsBeforeGamepad + 1) {
		throw new Error('gamepad A+Up did not produce one combined input update');
	}
	runAnimationFrameCycle();
	if (inputMessageCount() !== inputsBeforeGamepad + 1)
		throw new Error('unchanged gamepad state produced duplicate input updates');
	windowListeners.get('blur')();
	const inputsAfterBlur = inputMessageCount();
	runAnimationFrameCycle();
	if (latestInputMask() !== 0 ||
	    inputMessageCount() !== inputsAfterBlur) {
		throw new Error('held gamepad input returned while the window was unfocused');
	}
	windowListeners.get('focus')();
	runAnimationFrameCycle();
	if (latestInputMask() !== 272 ||
	    inputMessageCount() !== inputsAfterBlur + 1) {
		throw new Error('held gamepad input did not resume after window focus');
	}
	gamepads = [];
	runAnimationFrameCycle();
	if (latestInputMask() !== 0 ||
	    inputMessageCount() !== inputsAfterBlur + 2) {
		throw new Error('gamepad disconnect did not release its controls once');
	}

	/* Start the raw burst with a fresh FPS sampling window. */
	advanceFpsSample(1000);
	const rawPaintsBeforeBurst = putImageDataCount;
	lastWebSocket.onmessage({ data: rawPacket(0xf800) });
	lastWebSocket.onmessage({ data: rawPacket(0x07e0) });
	lastWebSocket.onmessage({ data: rawPacket(0x001f) });
	if (putImageDataCount !== rawPaintsBeforeBurst)
		throw new Error('raw video was converted synchronously in WebSocket.onmessage');
	runAnimationFrameCycle();
	if (putImageDataCount !== rawPaintsBeforeBurst + 1 ||
	    lastRawPixel.join(',') !== '0,0,255,255') {
		throw new Error('raw burst was not coalesced to its newest frame');
	}
	advanceFpsSample(1000);
	if (clientHooks.getFpsOsdState().value !== '1.0' ||
	    fpsOsdCurrentRects.length === 0) {
		throw new Error('FPS OSD counted received raw packets instead of one paint');
	}
	if (fpsOsdCurrentRects.some(rect => rect.x < 0))
		throw new Error('FPS OSD escaped the left edge of a narrow video frame');
	clientHooks.syncCanvasDimensions(256, 240);
	if (elements.get('fps-osd').width !== elements.get('screen').width ||
	    elements.get('fps-osd').height !== elements.get('screen').height) {
		throw new Error('FPS OSD dimensions did not follow the video canvas');
	}
	const expectedOuter = fpsOsdCurrentRects.some(rect =>
		rect.color === '#000000' && rect.x === 218 && rect.y === 4 &&
		rect.width === 5 && rect.height === 5
	);
	const expectedInner = fpsOsdCurrentRects.some(rect =>
		rect.color === '#303040' && rect.x === 219 && rect.y === 5 &&
		rect.width === 3 && rect.height === 3
	);
	const expectedForeground = fpsOsdCurrentRects.some(rect =>
		rect.color === '#ffffff' && rect.x === 220 && rect.y === 6 &&
		rect.width === 1 && rect.height === 1
	);
	if (!expectedOuter || !expectedInner || !expectedForeground ||
	    fpsOsdAlpha !== true || clientHooks.getFpsOsdState().value.includes('FPS')) {
		throw new Error('FPS OSD did not use a transparent pixel renderer');
	}
	const samplerBeforeRepeatedStatus = Array.from(intervals.values())[0];
	lastWebSocket.onmessage({
		data: JSON.stringify({ t: 'status', show_fps: true,
			game_loaded: true,
			rom_path: '/etc/nes-emulator/roms/Super Mario Bros.nes' })
	});
	if (intervals.size !== 1 ||
	    Array.from(intervals.values())[0] !== samplerBeforeRepeatedStatus ||
	    clientHooks.getFpsOsdState().value !== '1.0') {
		throw new Error('repeated status reset the FPS OSD or duplicated its sampler');
	}

	const staleSampler = samplerBeforeRepeatedStatus;
	lastWebSocket.onmessage({
		data: JSON.stringify({ t: 'status', show_fps: false,
			game_loaded: true,
			rom_path: '/etc/nes-emulator/roms/Super Mario Bros.nes' })
		});
	if (!elements.get('fps-osd').hidden || intervals.size !== 0 ||
	    clientHooks.getFpsOsdState().value !== '' ||
	    fpsOsdCurrentRects.length !== 0) {
		throw new Error('disabled FPS OSD remained visible or kept sampling');
	}
	lastWebSocket.onmessage({ data: rawPacket(0xf800) });
	runAnimationFrameCycle();
	lastWebSocket.onmessage({
		data: JSON.stringify({ t: 'status', show_fps: true,
			game_loaded: true,
			rom_path: '/etc/nes-emulator/roms/Super Mario Bros.nes' })
	});
	if (elements.get('fps-osd').hidden ||
	    clientHooks.getFpsOsdState().value !== '' ||
	    fpsOsdCurrentRects.length !== 0 || intervals.size !== 1) {
		throw new Error('re-enabled FPS OSD did not begin with a blank sample');
	}
	staleSampler();
	if (clientHooks.getFpsOsdState().value !== '' ||
	    fpsOsdCurrentRects.length !== 0 || intervals.size !== 1) {
		throw new Error('stale FPS sampler resurrected the OSD');
	}
	advanceFpsSample(1000);
	if (clientHooks.getFpsOsdState().value !== '0.0' ||
	    intervals.size !== 1) {
		throw new Error('disabled paints leaked into a later FPS sample');
	}

	function jpegPacket(marker) {
		const packet = new ArrayBuffer(13);
		const bytes = new Uint8Array(packet);
		const view = new DataView(packet);
		bytes[0] = 3;
		view.setUint16(2, marker, true);
		view.setUint16(4, 1, true);
		view.setUint32(6, 1, true);
		bytes[12] = marker;
		return packet;
	}
	lastWebSocket.onmessage({ data: jpegPacket(1) });
	lastWebSocket.onmessage({ data: jpegPacket(2) });
	lastWebSocket.onmessage({ data: jpegPacket(3) });
	if (jpegResolvers.length !== 1)
		throw new Error('JPEG client started more than one concurrent decode');
	jpegResolvers[0]();
	await new Promise(resolve => setImmediate(resolve));
	if (jpegResolvers.length !== 2)
		throw new Error('JPEG client did not retain the newest pending frame');
	jpegResolvers[1]();
	await new Promise(resolve => setImmediate(resolve));
	if (drawImageCount !== 2 ||
	    drawnWidths.length !== 2 ||
	    drawnWidths[0] !== 1 || drawnWidths[1] !== 3) {
		throw new Error('JPEG decode queue starved progress or retained an old pending frame');
	}
	if (elements.get('fps-osd').width !== elements.get('screen').width ||
	    elements.get('fps-osd').height !== elements.get('screen').height) {
		throw new Error('FPS OSD dimensions did not follow the video canvas');
	}
	advanceFpsSample(1000);
	if (clientHooks.getFpsOsdState().value !== '2.0')
		throw new Error('FPS OSD did not count the two displayed JPEG frames');
	advanceFpsSample(1000);
	if (clientHooks.getFpsOsdState().value !== '0.0')
		throw new Error('FPS OSD retained a stale value while frames stopped');

	lastWebSocket.onmessage({ data: jpegPacket(4) });
	if (jpegResolvers.length !== 3)
		throw new Error('JPEG reconnect test did not start an in-flight decode');
	const rawPaintsBeforeClose = putImageDataCount;
	lastWebSocket.onmessage({ data: rawPacket(0xf800) });
	const samplerBeforeClose = Array.from(intervals.values())[0];
	lastWebSocket.onclose();
	if (intervals.size !== 0 ||
	    clientHooks.getFpsOsdState().value !== '' ||
	    fpsOsdCurrentRects.length !== 0) {
		throw new Error('WebSocket close did not reset the FPS sampler');
	}
	samplerBeforeClose();
	if (clientHooks.getFpsOsdState().value !== '' ||
	    fpsOsdCurrentRects.length !== 0) {
		throw new Error('stale FPS sampler resurrected the OSD');
	}
	runAnimationFrameCycle();
	jpegResolvers[2]();
	await new Promise(resolve => setImmediate(resolve));
	if (drawImageCount !== 2)
		throw new Error('JPEG frame from a closed WebSocket was rendered');
	if (putImageDataCount !== rawPaintsBeforeClose)
		throw new Error('queued raw frame from a closed WebSocket was rendered');

	elements.get('rom-list').value =
		'/etc/nes-emulator/roms/Super Mario Bros.nes';
	await elements.get('btn-load').onclick();
	if (websocketCount !== 2 || !lastWebSocket)
		throw new Error('explicit restart did not create one fresh WebSocket');
	lastWebSocket.readyState = WebSocketMock.OPEN;
	lastWebSocket.onopen();
	lastWebSocket.onmessage({
		data: JSON.stringify({ t: 'status', show_fps: true,
			game_loaded: true,
			rom_path: '/etc/nes-emulator/roms/Super Mario Bros.nes' })
		});
	if (intervals.size !== 1 ||
	    clientHooks.getFpsOsdState().value !== '' ||
	    fpsOsdCurrentRects.length !== 0) {
		throw new Error('reconnect did not start one fresh FPS sample');
	}

	/* Heartbeats carry the current input and validate a stable connection. */
	windowListeners.get('keydown')(keyboardEvent('KeyZ'));
	for (let second = 0; second < 5; second++) {
		advanceTimeouts(1000);
		const heartbeats = heartbeatMessages();
		if (heartbeats.length !== second + 1 || heartbeats.at(-1).mask !== 1 ||
		    heartbeats.at(-1).seq !== second + 1)
			throw new Error('heartbeat cadence did not refresh the held input mask');
		acknowledgeHeartbeat();
	}
	windowListeners.get('keyup')(keyboardEvent('KeyZ'));

	/* Normal short audio jitter is buffered; an excessive lead replaces it. */
	if (!lastAudioContext)
		throw new Error('audio context was not created by a game-start gesture');
	lastAudioContext.state = 'running';
	const audioStart = audioSources.length;
	lastWebSocket.onmessage({ data: audioPacket() });
	lastWebSocket.onmessage({ data: audioPacket() });
	if (audioSources[audioStart].stopped || audioSources[audioStart + 1].stopped)
		throw new Error('normal audio jitter incorrectly reset scheduled sources');
	lastWebSocket.onmessage({ data: audioPacket() });
	if (!audioSources[audioStart].stopped || !audioSources[audioStart + 1].stopped ||
	    audioSources[audioStart + 2].stopped) {
		throw new Error('excessive audio lead overlapped stale scheduled sources');
	}
	/* A slightly late packet must rebase once, then remain sample-contiguous. */
	lastAudioContext.currentTime = .12;
	lastWebSocket.onmessage({ data: audioPacket() });
	const recoveredAudio = audioSources[audioStart + 3];
	if (!recoveredAudio || recoveredAudio.startedAt < .139 ||
	    audioSources[audioStart + 2].stopped) {
		throw new Error('audio underrun was scheduled in the past or cut a live source');
	}
	lastAudioContext.currentTime = .16;
	lastWebSocket.onmessage({ data: audioPacket() });
	const continuousAudio = audioSources[audioStart + 4];
	if (!continuousAudio || Math.abs(continuousAudio.startedAt -
	    (recoveredAudio.startedAt + .08)) > 1e-9) {
		throw new Error('audio recovery overlapped or separated consecutive PCM blocks');
	}
	elements.get('btn-mute').onclick({ currentTarget: elements.get('btn-mute') });
	if (!audioSources[audioStart + 2].stopped || !recoveredAudio.stopped ||
	    !continuousAudio.stopped)
		throw new Error('mute did not cancel already scheduled audio');
	elements.get('btn-mute').onclick({ currentTarget: elements.get('btn-mute') });

	/* First hung decoder forces one bounded reconnect and leaves one quarantined slot. */
	const hungDecode = jpegResolvers.length;
	const hungDecodePaints = drawImageCount;
	const firstHungSocket = lastWebSocket;
	const socketsBeforeJpegRecovery = websocketCount;
	lastWebSocket.onmessage({ data: jpegPacket(5) });
	lastWebSocket.onmessage({ data: jpegPacket(6) });
	if (jpegResolvers.length !== hungDecode + 1)
		throw new Error('hung JPEG test started concurrent decoders');
	advanceTimeouts(2000);
	await new Promise(resolve => setImmediate(resolve));
	if (firstHungSocket.closeCount < 1 || websocketCount !== socketsBeforeJpegRecovery ||
	    jpegResolvers.length !== hungDecode + 1 || drawImageCount !== hungDecodePaints ||
	    !elements.get('status').textContent.includes('JPEG decoder stalled')) {
		throw new Error('first JPEG timeout did not force one bounded transport recovery');
	}
	firstHungSocket.onmessage({ data: jpegPacket(12) });
	if (jpegResolvers.length !== hungDecode + 1)
		throw new Error('abandoned JPEG epoch accepted another decoder attempt');
	advanceTimeouts(700);
	if (websocketCount !== socketsBeforeJpegRecovery + 1)
		throw new Error('JPEG timeout reconnect did not create a fresh transport epoch');
	lastWebSocket.readyState = WebSocketMock.OPEN;
	lastWebSocket.onopen();
	lastWebSocket.onmessage({ data: jpegPacket(13) });
	if (jpegResolvers.length !== hungDecode + 2)
		throw new Error('old quarantined JPEG decode blocked the new transport epoch');
	jpegResolvers[hungDecode + 1]();
	await new Promise(resolve => setImmediate(resolve));
	if (drawImageCount !== hungDecodePaints + 1 || drawnWidths.at(-1) !== 13)
		throw new Error('fresh transport did not paint around an old hung JPEG decode');

	/* Second permanent hang fills the cap: show an error, never reconnect-loop. */
	lastWebSocket.onmessage({ data: jpegPacket(14) });
	if (jpegResolvers.length !== hungDecode + 3)
		throw new Error('bounded JPEG test did not start its second decode slot');
	advanceTimeouts(2000);
	await new Promise(resolve => setImmediate(resolve));
	const cappedSocket = lastWebSocket;
	const socketsAtJpegCap = websocketCount;
	if (cappedSocket.closeCount !== 0 ||
	    !elements.get('status').textContent.includes('JPEG decoder unavailable')) {
		throw new Error('second permanent JPEG hang did not stop at the explicit cap');
	}
	acknowledgeHeartbeat(cappedSocket);
	for (let second = 0; second < 4; second++) {
		cappedSocket.onmessage({ data: jpegPacket(15 + second) });
		advanceTimeouts(1000);
		acknowledgeHeartbeat(cappedSocket);
	}
	if (websocketCount !== socketsAtJpegCap || jpegResolvers.length !== hungDecode + 3 ||
	    cappedSocket.closeCount !== 0) {
		throw new Error('JPEG global cap leaked decoders or entered a reconnect storm');
	}

	/* ACK may lag the latest send, while stale and out-of-order ACKs are ignored. */
	advanceTimeouts(1000);
	const laggingSeq = latestHeartbeat(cappedSocket).seq;
	advanceTimeouts(1000);
	const newerSeq = latestHeartbeat(cappedSocket).seq;
	acknowledgeHeartbeat(cappedSocket, laggingSeq);
	if (clientHooks.getHeartbeatState().accepted !== laggingSeq ||
	    !clientHooks.getHeartbeatState().pending.includes(newerSeq)) {
		throw new Error('fresh lagging heartbeat ACK was incorrectly rejected');
	}
	advanceTimeouts(1000);
	const newestSeq = latestHeartbeat(cappedSocket).seq;
	acknowledgeHeartbeat(cappedSocket, newestSeq);
	acknowledgeHeartbeat(cappedSocket, newerSeq);
	if (clientHooks.getHeartbeatState().accepted !== newestSeq)
		throw new Error('out-of-order heartbeat ACK moved liveness backwards');

	/* An OPEN-but-silent transport is abandoned without waiting for onclose. */
	lastWebSocket.onmessage({ data: audioPacket() });
	const silenceAudio = audioSources.at(-1);
	const silentSocket = lastWebSocket;
	const socketsBeforeSilence = websocketCount;
	let deeplyStaleSeq = 0;
	for (let second = 0; second < 9; second++) {
		silentSocket.onmessage({ data: rawPacket(0x07e0) });
		runAnimationFrameCycle();
		advanceTimeouts(1000);
		if (second === 0)
			deeplyStaleSeq = latestHeartbeat(silentSocket).seq;
		if (second === 4)
			acknowledgeHeartbeat(silentSocket, deeplyStaleSeq);
	}
	if (silentSocket.closeCount < 1 || websocketCount !== socketsBeforeSilence ||
	    !silenceAudio.stopped ||
	    !elements.get('status').textContent.includes('stalled')) {
		throw new Error('missing heartbeat ACK survived downstream frames or retained audio');
	}
	const reconnectAfterSilence = Array.from(timeouts.values())
		.map(timer => timer.at - timeoutClock);
	if (reconnectAfterSilence.length !== 1 || reconnectAfterSilence[0] < 375 ||
	    reconnectAfterSilence[0] > 625) {
		throw new Error('stable connection did not reset randomized reconnect backoff');
	}
	const timersBeforeLateClose = timeouts.size;
	silentSocket.onclose();
	if (timeouts.size !== timersBeforeLateClose)
		throw new Error('late close event duplicated a forced reconnect');
	advanceTimeouts(700);
	if (websocketCount !== socketsBeforeSilence + 1)
		throw new Error('silence watchdog did not create a fresh transport');
	acknowledgeHeartbeat(silentSocket, deeplyStaleSeq);
	if (clientHooks.getHeartbeatState().accepted !== 0)
		throw new Error('delayed ACK from an old socket contaminated a new generation');

	/* A CONNECTING socket and its late close cannot hold the client forever. */
	const hungSocket = lastWebSocket;
	advanceTimeouts(9000);
	if (hungSocket.closeCount < 1 || websocketCount !== socketsBeforeSilence + 1 ||
	    !elements.get('status').textContent.includes('timed out')) {
		throw new Error('CONNECTING deadline did not force-abandon the hung socket');
	}
	const timersBeforeHungLateClose = timeouts.size;
	hungSocket.onclose();
	if (timeouts.size !== timersBeforeHungLateClose)
		throw new Error('hung socket late close duplicated its retry');
	advanceTimeouts(1300);
	if (websocketCount !== socketsBeforeSilence + 2)
		throw new Error('client did not retry after a hung CONNECTING socket');

	/* One early status validates syntax, but must not reset a flapping backoff. */
	lastWebSocket.readyState = WebSocketMock.OPEN;
	lastWebSocket.onopen();
	advanceTimeouts(1000);
	acknowledgeHeartbeat();
	const earlyReplySocket = lastWebSocket;
	earlyReplySocket.onclose();
	const flapDelays = Array.from(timeouts.values())
		.map(timer => timer.at - timeoutClock);
	if (flapDelays.length !== 1 || flapDelays[0] < 1500 || flapDelays[0] > 2500)
		throw new Error('one early reply incorrectly reset reconnect backoff');
	advanceTimeouts(2500);
	if (websocketCount !== socketsBeforeSilence + 3)
		throw new Error('flapping-backoff retry did not run');

	/* send() exceptions also abandon immediately, independent of onclose. */
	lastWebSocket.readyState = WebSocketMock.OPEN;
	lastWebSocket.onopen();
	const writeFailureSocket = lastWebSocket;
	writeFailureSocket.throwOnSend = true;
	advanceTimeouts(1000);
	if (writeFailureSocket.closeCount < 1 ||
	    !elements.get('status').textContent.includes('write failed')) {
		throw new Error('WebSocket send exception left the failed transport active');
	}
	const timersBeforeWriteLateClose = timeouts.size;
	writeFailureSocket.onclose();
	if (timeouts.size !== timersBeforeWriteLateClose)
		throw new Error('write-failure late close duplicated its retry');
	advanceTimeouts(5000);
	if (websocketCount !== socketsBeforeSilence + 4)
		throw new Error('client did not retry after a WebSocket send exception');

	/* Browser-side WebSocket buffering is bounded before it becomes stale input. */
	lastWebSocket.readyState = WebSocketMock.OPEN;
	lastWebSocket.onopen();
	const congestedSocket = lastWebSocket;
	const socketsBeforeCongestion = websocketCount;
	congestedSocket.bufferedAmount = 1024 * 1024;
	advanceTimeouts(1000);
	if (congestedSocket.closeCount < 1 ||
	    !elements.get('status').textContent.includes('output congested')) {
		throw new Error('excessive WebSocket bufferedAmount was allowed to grow');
	}
	advanceTimeouts(8000);
	if (websocketCount !== socketsBeforeCongestion + 1)
		throw new Error('client did not recover from excessive browser buffering');

	/* A false offline hint must not tear down a working LAN transport or audio. */
	lastWebSocket.readyState = WebSocketMock.OPEN;
	lastWebSocket.onopen();
	const onlineSocket = lastWebSocket;
	onlineSocket.onmessage({ data: audioPacket() });
	const offlineHintAudioSource = audioSources.at(-1);
	windowListeners.get('keydown')(keyboardEvent('KeyZ'));
	const writesBeforeFalseOffline = onlineSocket.sent.length;
	navigatorMock.onLine = false;
	windowListeners.get('offline')();
	if (onlineSocket.sent.length !== writesBeforeFalseOffline)
		throw new Error('false offline hint released live LAN controller input');
	const socketsBeforeOfflineProbe = websocketCount;
	advanceTimeouts(1000);
	acknowledgeHeartbeat(onlineSocket);
	await new Promise(resolve => setImmediate(resolve));
	if (websocketCount !== socketsBeforeOfflineProbe || onlineSocket.closeCount !== 0 ||
	    lastAudioContext.state !== 'running' || offlineHintAudioSource.stopped) {
		throw new Error('false offline hint tore down a proven LAN transport or audio');
	}
	windowListeners.get('keyup')(keyboardEvent('KeyZ'));

	/* With no transport, an explicit offline transition probes immediately once. */
	onlineSocket.readyState = WebSocketMock.CLOSED;
	windowListeners.get('offline')();
	advanceTimeouts(0);
	if (websocketCount !== socketsBeforeOfflineProbe + 1 ||
	    !elements.get('status').textContent.includes('probing the router')) {
		throw new Error('offline transition without a transport did not probe immediately');
	}
	const offlineProbeSocket = lastWebSocket;
	windowListeners.get('offline')();
	advanceTimeouts(0);
	if (websocketCount !== socketsBeforeOfflineProbe + 1 ||
	    offlineProbeSocket.closeCount !== 0) {
		throw new Error('repeated offline hint replaced an active CONNECTING LAN probe');
	}
	offlineProbeSocket.readyState = WebSocketMock.OPEN;
	offlineProbeSocket.onopen();
	await new Promise(resolve => setImmediate(resolve));
	advanceTimeouts(1000);
	acknowledgeHeartbeat(offlineProbeSocket);
	if (offlineProbeSocket.closeCount !== 0 ||
	    lastAudioContext.state !== 'running' ||
	    !elements.get('status').textContent.includes('connected')) {
		throw new Error('reachable LAN transport was rejected while navigator stayed offline');
	}

	/* A failed offline probe waits another 6–8 seconds rather than storming. */
	offlineProbeSocket.readyState = WebSocketMock.CLOSED;
	offlineProbeSocket.onclose();
	const socketsBeforeSecondOfflineProbe = websocketCount;
	advanceTimeouts(5000);
	if (websocketCount !== socketsBeforeSecondOfflineProbe)
		throw new Error('offline reconnect probe ignored its low-frequency floor');
	advanceTimeouts(3000);
	if (websocketCount !== socketsBeforeSecondOfflineProbe + 1)
		throw new Error('offline reconnect probe exceeded its capped delay');
	lastWebSocket.readyState = WebSocketMock.CLOSED;
	lastWebSocket.onclose();

	/* A real online event still retries immediately and is constructor-safe. */
	const socketsBeforeOnlineRetry = websocketCount;
	navigatorMock.onLine = true;
	failNextWebSocketConstruction = true;
	windowListeners.get('online')();
	windowListeners.get('online')();
	advanceTimeouts(0);
	if (websocketCount !== socketsBeforeOnlineRetry ||
	    !elements.get('status').textContent.includes('connection failed')) {
		throw new Error('online retry did not safely handle constructor failure');
	}
	advanceTimeouts(8000);
	await new Promise(resolve => setImmediate(resolve));
	if (websocketCount !== socketsBeforeOnlineRetry + 1)
		throw new Error('constructor failure did not retain a capped retry');

	/* Visibility and BFCache restore each resume exactly one desired stream. */
	lastWebSocket.readyState = WebSocketMock.OPEN;
	lastWebSocket.onopen();
	const visibilityAudioContext = lastAudioContext;
	const visibilitySuspendCount = visibilityAudioContext.suspendCount;
	const visibilityResumeCount = visibilityAudioContext.resumeCount;
	lastWebSocket.onmessage({ data: audioPacket() });
	const visibilityAudioSource = audioSources.at(-1);
	const socketsBeforeVisibility = websocketCount;
	documentMock.hidden = true;
	documentListeners.get('visibilitychange')();
	if (intervals.size !== 0 ||
	    clientHooks.getFpsOsdState().value !== '' ||
	    fpsOsdCurrentRects.length !== 0 ||
	    visibilityAudioContext.suspendCount !== visibilitySuspendCount + 1 ||
	    !visibilityAudioSource.stopped) {
		throw new Error('hiding an active game did not suspend FPS, audio and transport');
	}
	navigatorMock.onLine = false;
	documentMock.hidden = false;
	documentListeners.get('visibilitychange')();
	await new Promise(resolve => setImmediate(resolve));
	if (websocketCount !== socketsBeforeVisibility ||
	    visibilityAudioContext !== lastAudioContext ||
	    visibilityAudioContext.resumeCount !== visibilityResumeCount ||
	    visibilityAudioContext.state !== 'suspended' ||
	    !elements.get('status').textContent.includes('offline')) {
		throw new Error('visible transition ignored the resampled offline audio intent');
	}
	navigatorMock.onLine = true;
	documentListeners.get('visibilitychange')();
	documentListeners.get('visibilitychange')();
	await new Promise(resolve => setImmediate(resolve));
	if (websocketCount !== socketsBeforeVisibility + 1 ||
	    visibilityAudioContext.resumeCount !== visibilityResumeCount + 1 ||
	    visibilityAudioContext.state !== 'running') {
		throw new Error('visibility restore did not create one stream and one audio resume');
	}

	/* Duplicate resumes coalesce; hiding before async resume settles re-suspends it. */
	const socketsBeforeResumeRace = websocketCount;
	documentMock.hidden = true;
	documentListeners.get('visibilitychange')();
	deferAudioResume = true;
	documentMock.hidden = false;
	documentListeners.get('visibilitychange')();
	documentListeners.get('visibilitychange')();
	await new Promise(resolve => setImmediate(resolve));
	if (typeof pendingAudioResumeResolve !== 'function' ||
	    visibilityAudioContext.resumeCount !== visibilityResumeCount + 2 ||
	    websocketCount !== socketsBeforeResumeRace + 1) {
		throw new Error('duplicate visibility restore did not coalesce async audio resume');
	}
	documentMock.hidden = true;
	documentListeners.get('visibilitychange')();
	pendingAudioResumeResolve();
	pendingAudioResumeResolve = null;
	deferAudioResume = false;
	await new Promise(resolve => setImmediate(resolve));
	if (visibilityAudioContext.state !== 'suspended')
		throw new Error('audio resumed after the page became hidden mid-resume');
	documentMock.hidden = false;
	documentListeners.get('visibilitychange')();
	documentListeners.get('visibilitychange')();
	await new Promise(resolve => setImmediate(resolve));
	if (websocketCount !== socketsBeforeResumeRace + 2 ||
	    visibilityAudioContext.state !== 'running') {
		throw new Error('audio/transport did not recover after the cancelled resume race');
	}

	/* A stale resume must reconcile the latest rapid hide/show audio intent. */
	const socketsBeforeRapidVisibility = websocketCount;
	const suspendBeforeRapidVisibility = visibilityAudioContext.suspendCount;
	const resumeBeforeRapidVisibility = visibilityAudioContext.resumeCount;
	deferAudioSuspend = true;
	deferAudioResume = true;
	documentMock.hidden = true;
	documentListeners.get('visibilitychange')();
	documentMock.hidden = false;
	documentListeners.get('visibilitychange')();
	documentMock.hidden = true;
	documentListeners.get('visibilitychange')();
	documentMock.hidden = false;
	documentListeners.get('visibilitychange')();
	if (typeof pendingAudioSuspendResolve !== 'function')
		throw new Error('rapid visibility test did not retain its async suspension');
	pendingAudioSuspendResolve();
	pendingAudioSuspendResolve = null;
	deferAudioSuspend = false;
	await new Promise(resolve => setImmediate(resolve));
	if (typeof pendingAudioResumeResolve !== 'function')
		throw new Error('stale audio resume did not reconcile the latest visible intent');
	pendingAudioResumeResolve();
	pendingAudioResumeResolve = null;
	deferAudioResume = false;
	await new Promise(resolve => setImmediate(resolve));
	if (visibilityAudioContext.suspendCount !== suspendBeforeRapidVisibility + 1 ||
	    visibilityAudioContext.resumeCount !== resumeBeforeRapidVisibility + 1 ||
	    visibilityAudioContext.state !== 'running' ||
	    websocketCount !== socketsBeforeRapidVisibility + 2) {
		throw new Error('rapid hide/show sequence left audio or transport in a stale state');
	}
	lastWebSocket.readyState = WebSocketMock.OPEN;
	lastWebSocket.onopen();
	lastWebSocket.onmessage({
		data: JSON.stringify({ t: 'status', show_fps: true,
			game_loaded: true,
			rom_path: '/etc/nes-emulator/roms/Super Mario Bros.nes' })
	});
	lastWebSocket.onmessage({ data: rawPacket(0x07e0) });
	runAnimationFrameCycle();
	advanceFpsSample(1000);
	if (intervals.size !== 1 || clientHooks.getFpsOsdState().value !== '1.0' ||
	    fpsOsdCurrentRects.length === 0) {
		throw new Error('BFCache test could not establish an active FPS OSD');
	}
	const cachedFpsSampler = Array.from(intervals.values())[0];
	lastWebSocket.onmessage({ data: audioPacket() });
	const cachedAudioSource = audioSources.at(-1);
	const cachedAudioContext = lastAudioContext;
	const cachedSuspendCount = cachedAudioContext.suspendCount;
	const cachedResumeCount = cachedAudioContext.resumeCount;
	const cachedCloseCount = cachedAudioContext.closeCount;
	const socketsBeforePageCache = websocketCount;
	const preferencesBeforePageCache = displayPreferenceRequests;
	documentMock.fullscreenElement = elements.get('game-stage');
	windowListeners.get('resize')();
	if (!elements.get('screen-frame').style.getPropertyValue('width'))
		throw new Error('fullscreen resize test did not schedule fitted geometry');
	/* Simulate an external exit whose change event is delayed past pagehide. */
	documentMock.fullscreenElement = null;
	fullscreenRequestHangs = true;
	deferAudioSuspend = true;
	const cachedFullscreen = elements.get('btn-fullscreen').onclick();
	windowListeners.get('pagehide')({ persisted: true });
	if (await cachedFullscreen !== false || elements.get('btn-fullscreen').disabled ||
	    elements.get('game-stage').classList.contains('fullscreen-active') ||
	    cachedAudioContext.closeCount !== cachedCloseCount ||
	    cachedAudioContext.suspendCount !== cachedSuspendCount + 1 ||
	    !cachedAudioSource.stopped ||
	    intervals.size !== 0 || clientHooks.getFpsOsdState().value !== '' ||
	    fpsOsdCurrentRects.length !== 0 ||
	    elements.get('screen-frame').style.getPropertyValue('width') !== '') {
		throw new Error('BFCache pagehide did not settle fullscreen or preserve suspended audio');
	}
	cachedFpsSampler();
	if (clientHooks.getFpsOsdState().value !== '' ||
	    fpsOsdCurrentRects.length !== 0 || intervals.size !== 0) {
		throw new Error('stale FPS sampler resurrected the OSD');
	}
	runAnimationFrameCycle();
	if (elements.get('screen-frame').style.getPropertyValue('width') !== '')
		throw new Error('late fullscreen fit RAF restored stale geometry after pagehide');
	fullscreenRequestHangs = false;
	documentListeners.get('fullscreenchange')({ type: 'fullscreenchange' });
	if (elements.get('display-message').textContent !== '')
		throw new Error('late fullscreenchange retained a stale transition error');
	windowListeners.get('pageshow')({ persisted: true });
	windowListeners.get('pageshow')({ persisted: true });
	if (typeof pendingAudioSuspendResolve !== 'function')
		throw new Error('BFCache test did not retain its pending audio suspension');
	pendingAudioSuspendResolve();
	pendingAudioSuspendResolve = null;
	deferAudioSuspend = false;
	await new Promise(resolve => setImmediate(resolve));
	if (websocketCount !== socketsBeforePageCache + 1 ||
	    displayPreferenceRequests !== preferencesBeforePageCache + 1 ||
	    lastWebSocket.url !==
		'ws://192.168.1.1:9090/ws?token=0123456789abcdef0123456789abcdef' ||
	    lastAudioContext !== cachedAudioContext ||
	    cachedAudioContext.resumeCount !== cachedResumeCount + 1 ||
	    cachedAudioContext.state !== 'running' ||
	    elements.get('game-stage').classList.contains('fullscreen-active') ||
	    elements.get('screen-frame').style.getPropertyValue('width') !== '') {
		throw new Error(
			'persisted pageshow did not restore exactly one stream, preference probe and audio context'
		);
	}
	lastWebSocket.readyState = WebSocketMock.OPEN;
	lastWebSocket.onopen();
	lastWebSocket.onmessage({
		data: JSON.stringify({ t: 'status', show_fps: true,
			game_loaded: true,
			rom_path: '/etc/nes-emulator/roms/Super Mario Bros.nes' })
	});
	if (intervals.size !== 1 || clientHooks.getFpsOsdState().value !== '' ||
	    fpsOsdCurrentRects.length !== 0) {
		throw new Error('BFCache restore did not start one blank fresh FPS sampler');
	}
	windowListeners.get('pagehide')({ persisted: false });
	if (cachedAudioContext.closeCount !== cachedCloseCount + 1 ||
	    lastAudioContext !== cachedAudioContext || cachedAudioContext.state !== 'closed') {
		throw new Error('non-persisted pagehide did not close its audio context');
	}

	process.stdout.write('Play lifecycle contract: OK\n');
})().catch(error => {
	console.error(error);
	process.exitCode = 1;
});
