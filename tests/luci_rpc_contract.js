'use strict';

const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
const source = fs.readFileSync(path.join(
	root,
	'package/luci-app-nes-emulator/htdocs/luci-static/resources/view/' +
		'nes-emulator/overview.js'
), 'utf8');
const playSource = fs.readFileSync(path.join(
	root,
	'package/luci-app-nes-emulator/htdocs/luci-static/resources/view/' +
		'nes-emulator/play.js'
), 'utf8');

if (typeof String.prototype.format !== 'function') {
	Object.defineProperty(String.prototype, 'format', {
		value(...values) {
			let index = 0;
			return this.replace(/%s/g, () => String(values[index++]));
		}
	});
}

const responses = {
	'session.access': { access: true },
	'nes-emulator.status': {
		core: 'FCEUmm',
		rom: '',
		running: false,
		paused: false,
		stream_format: 'rgb565',
		fps: 60,
		stream_fps: 2
	},
	'nes-emulator.roms': {
		roms: [{
			name: 'Super Mario Bros.nes',
			path: '/etc/nes-emulator/roms/Super Mario Bros.nes',
			readable: true
		}, {
			name: 'root-only.nes',
			path: '/etc/nes-emulator/roms/root-only.nes',
			readable: false,
			error: 'set group nesd and mode 0640'
		}]
	}
};
const calls = [];
const notifications = [];
const luci = {
	env: { rpctimeout: 23 },
	url: value => '/' + value
};
let uploadedFile = {
	name: 'Super Mario Bros.nes'
};

function applyExpect(expect, response) {
	const keys = Object.keys(expect || {});
	if (keys.length === 0)
		return undefined;
	if (keys.length === 1) {
		const key = keys[0];
		if (key === '')
			return response === undefined ? expect[key] : response;
		return response && response[key] !== undefined
			? response[key]
			: expect[key];
	}
	return response;
}

const rpc = {
	declare(specification) {
		return async (...args) => {
			const method =
				specification.object + '.' + specification.method;
			calls.push({
				method,
				args,
				rpcTimeout: luci.env.rpctimeout
			});
			const configured = responses[method];
			const response = typeof configured === 'function'
				? await configured(...args)
				: await configured;
			return applyExpect(
				specification.expect,
				response
			);
		};
	}
};

const nodes = new Map();
let actionButtons = [];
const documentMock = {
	getElementById(id) {
		if (!nodes.has(id)) {
			nodes.set(id, {
				children: [],
				replaceChildren(...children) {
					this.children = children;
				}
			});
		}
		return nodes.get(id);
	},
	querySelectorAll(selector) {
		return selector === '[data-nes-action]' ? actionButtons : [];
	}
};

function actionButton(kind, unavailable) {
	const attributes = {
		'data-nes-kind': kind,
		'data-nes-unavailable': unavailable ? '1' : '0'
	};
	return {
		attributes,
		disabled: false,
		textContent: '',
		getAttribute(name) {
			return attributes[name] === undefined ? null : attributes[name];
		}
	};
}

function E(tag, attributes, children) {
	if (children === undefined && (
		typeof attributes === 'string' ||
		Array.isArray(attributes) ||
		attributes === null
	)) {
		children = attributes;
		attributes = {};
	}
	return { tag, attributes: attributes || {}, children: children || [] };
}

const factory = new Function(
	'view', 'rpc', 'ui', 'E', '_', 'L', 'window', 'document',
	'requestAnimationFrame',
	source
);
const component = factory(
	{ extend: value => value },
	rpc,
	{
		createHandlerFn: () => () => {},
		addNotification: (title, content, level) => {
			notifications.push({ title, content, level });
		},
		uploadFile: async () => uploadedFile
	},
	E,
	value => value,
	luci,
	{ location: { protocol: 'http:', hostname: '192.168.1.1' } },
	documentMock,
	() => 0
);

function deferred() {
	let resolve;
	let reject;
	const promise = new Promise((resolvePromise, rejectPromise) => {
		resolve = resolvePromise;
		reject = rejectPromise;
	});
	return { promise, resolve, reject };
}

function serializedNotification(notification) {
	return JSON.stringify(notification && notification.content);
}

(async () => {
	const access = await component.load();
	if (calls[0].rpcTimeout !== 23 || luci.env.rpctimeout !== 23)
		throw new Error('ordinary RPC call changed the global timeout');
	actionButtons = [
		actionButton('start'),
		actionButton('pause'),
		actionButton('reset'),
		actionButton('stop'),
		actionButton('upload'),
		actionButton('load'),
		actionButton('load', true)
	];
	const button = kind => actionButtons.find(item =>
		item.getAttribute('data-nes-kind') === kind
	);
	component.render(access);
	if (component.canControl !== true)
		throw new Error('write-capable LuCI session was rendered read-only');

	await component.onRefreshAll();
	if (!button('start').disabled ||
	    !button('pause').disabled ||
	    !button('reset').disabled ||
	    !button('stop').disabled ||
	    button('upload').disabled ||
	    button('load').disabled ||
	    !actionButtons[6].disabled) {
		throw new Error('no-ROM state enabled an invalid lifecycle action');
	}

	component.lastStatus = {
		game_loaded: true,
		rom: 'Super Mario Bros.nes',
		running: false,
		paused: false
	};
	component.syncActionButtons();
	if (button('start').disabled ||
	    !button('pause').disabled ||
	    !button('reset').disabled ||
	    !button('stop').disabled) {
		throw new Error('stopped loaded-ROM state has invalid controls');
	}

	component.lastStatus = {
		game_loaded: true,
		rom: 'Super Mario Bros.nes',
		running: true,
		paused: false
	};
	component.syncActionButtons();
	if (!button('start').disabled ||
	    button('pause').disabled ||
	    button('reset').disabled ||
	    button('stop').disabled ||
	    button('pause').textContent !== 'Pause') {
		throw new Error('running state has invalid controls');
	}
	component.lastStatus.paused = true;
	component.syncActionButtons();
	if (button('pause').textContent !== 'Resume')
		throw new Error('paused state did not expose a Resume action');
	component.setActionBusy(true);
	if (actionButtons.some(item => !item.disabled))
		throw new Error('busy state left an emulator action enabled');
	const busyRomTable = JSON.stringify(component.romTable([{
		name: 'busy.nes',
		path: '/etc/nes-emulator/roms/busy.nes',
		readable: true
	}]));
	if (!busyRomTable.includes('"disabled":true'))
		throw new Error('a refreshed ROM table bypassed the busy state');
	component.setActionBusy(false);
	if (button('pause').disabled || button('stop').disabled)
		throw new Error('leaving busy state lost the current status policy');

	const rendered = JSON.stringify(
		documentMock.getElementById('nes-rom-table').children
	);
	if (!rendered.includes('Super Mario Bros.nes'))
		throw new Error('ROM RPC result was rendered as an empty table');
	if (!rendered.includes('root-only.nes') ||
	    !rendered.includes('"data-nes-unavailable":"1"') ||
	    !rendered.includes('set group nesd and mode 0640')) {
		throw new Error('unreadable ROM is hidden or has an active Load button');
	}
	const status = JSON.stringify(
		documentMock.getElementById('nes-status').children
	);
	if (!status.includes('2 stream FPS') || status.includes('60 FPS'))
		throw new Error('status rendered native FPS as media stream FPS');

	const staged =
		'/tmp/nes-emulator-upload/0123456789abcdef0123456789abcdef.rom';
	responses['nes-emulator.reserve_upload'] = { staged };
	responses['nes-emulator.import'] = {
		ok: false,
		stored: true,
		path: '/etc/nes-emulator/roms/Super_Mario_Bros.nes',
		error: 'nesd could not be started'
	};
	responses['nes-emulator.discard_upload'] = { ok: true };
	const partialCallStart = calls.length;
	await component.onUpload();
	const partialCalls = calls.slice(partialCallStart);
	const importCall = partialCalls.find(call =>
		call.method === 'nes-emulator.import'
	);
	if (!importCall || importCall.rpcTimeout !== 120 ||
	    luci.env.rpctimeout !== 23) {
		throw new Error('ROM import lacks a restored 120-second RPC timeout');
	}
	if (partialCalls.some(call =>
		call.method === 'nes-emulator.discard_upload')) {
		throw new Error('stored ROM upload reservation was discarded again');
	}
	const partialNotice = notifications.find(notification =>
		notification.level === 'warning' &&
		serializedNotification(notification).includes(
			'ROM was stored but could not be loaded'
		)
	);
	if (!partialNotice ||
	    !serializedNotification(partialNotice).includes(
		    '/etc/nes-emulator/roms/Super_Mario_Bros.nes'
	    ) ||
	    notifications.some(notification =>
		    notification.level === 'error' &&
		    serializedNotification(notification).includes('Upload failed'))) {
		throw new Error('partial stored upload was reported as a failed upload');
	}
	if (component.actionBusy)
		throw new Error('partial stored upload left the view permanently busy');

	const oldStatus = deferred();
	const newStatus = deferred();
	const oldRoms = deferred();
	const newRoms = deferred();
	const statusQueue = [ oldStatus.promise, newStatus.promise ];
	const romQueue = [ oldRoms.promise, newRoms.promise ];
	responses['nes-emulator.status'] = () => statusQueue.shift();
	responses['nes-emulator.roms'] = () => romQueue.shift();
	const staleRefresh = component.onRefreshAll();
	const currentRefresh = component.onRefreshAll();
	newStatus.resolve({
		core: 'FCEUmm',
		game_loaded: true,
		rom: 'new.nes',
		running: true,
		paused: false,
		stream_fps: 2
	});
	newRoms.resolve({
		roms: [{
			name: 'new.nes',
			path: '/etc/nes-emulator/roms/new.nes',
			readable: true
		}]
	});
	await currentRefresh;
	oldStatus.resolve({
		core: 'FCEUmm',
		game_loaded: false,
		rom: '',
		running: false,
		paused: false,
		stream_fps: 2
	});
	oldRoms.resolve({
		roms: [{
			name: 'stale.nes',
			path: '/etc/nes-emulator/roms/stale.nes',
			readable: true
		}]
	});
	await staleRefresh;
	const newestStatus = JSON.stringify(
		documentMock.getElementById('nes-status').children
	);
	const newestRoms = JSON.stringify(
		documentMock.getElementById('nes-rom-table').children
	);
	if (!newestStatus.includes('new.nes') ||
	    newestStatus.includes('no ROM') ||
	    !newestRoms.includes('new.nes') ||
	    newestRoms.includes('stale.nes') ||
	    component.lastStatus.rom !== 'new.nes') {
		throw new Error('an older refresh overwrote newer daemon state');
	}

	responses['nes-emulator.status'] = {
		core: 'FCEUmm',
		game_loaded: true,
		rom: 'new.nes',
		running: true,
		paused: false,
		stream_fps: 2
	};
	responses['nes-emulator.roms'] = {
		roms: [{
			name: 'new.nes',
			path: '/etc/nes-emulator/roms/new.nes',
			readable: true
		}]
	};
	const romPath = '/etc/nes-emulator/roms/Super Mario Bros.nes';
	await component.onLoad(romPath);
	const loadCall = calls.find(call => call.method === 'nes-emulator.load');
	if (!loadCall || loadCall.args[0] !== romPath ||
	    loadCall.rpcTimeout !== 120 || luci.env.rpctimeout !== 23) {
		throw new Error('ROM path with spaces was not passed intact to RPC');
	}

	component.lastStatus = {
		game_loaded: true,
		rom: 'new.nes',
		running: false,
		paused: false
	};
	responses['nes-emulator.start'] = { ok: true };
	const startCallOffset = calls.length;
	await component.onStart();
	const startCall = calls.slice(startCallOffset).find(call =>
		call.method === 'nes-emulator.start'
	);
	if (!startCall || startCall.rpcTimeout !== 120 ||
	    luci.env.rpctimeout !== 23) {
		throw new Error('daemon start lacks a restored 120-second RPC timeout');
	}

	responses['session.access'] = { access: false };
	const readOnlyAccess = await component.load();
	component.render(readOnlyAccess);
	component.syncActionButtons();
	if (component.canControl !== false)
		throw new Error('read-only LuCI session received write controls');
	if (actionButtons.some(item => !item.disabled))
		throw new Error('read-only session retained an enabled action');
	const readOnlyTable = JSON.stringify(component.romTable(
		responses['nes-emulator.roms'].roms
	));
	if (!readOnlyTable.includes('"disabled":true'))
		throw new Error('read-only LuCI session can activate the Load button');
	const emptyReadOnlyTable = JSON.stringify(component.romTable([]));
	if (!emptyReadOnlyTable.includes('No ROMs found') ||
	    emptyReadOnlyTable.includes('upload one above')) {
		throw new Error('read-only empty state recommends an unavailable upload');
	}
	responses['nes-emulator.roms'] = {
		roms: [],
		notice: 'offline scan notice',
		truncated: true
	};
	await component.onRefreshAll();
	const diagnosticTable = JSON.stringify(
		documentMock.getElementById('nes-rom-table').children
	);
	if (!diagnosticTable.includes('ROM list was truncated') ||
	    !diagnosticTable.includes('offline scan notice')) {
		throw new Error('ROM scan notice or truncation state was not rendered');
	}

	const playNodes = new Map();
	const playDocument = {
		getElementById(id) {
			if (!playNodes.has(id)) {
				playNodes.set(id, {
					children: [],
					replaceChildren(...children) {
						this.children = children;
					}
				});
			}
			return playNodes.get(id);
		}
	};
	const playFactory = new Function(
		'view', 'rpc', 'E', '_', 'L', 'window', 'document',
		playSource
	);
	const playComponent = playFactory(
		{ extend: value => value },
		rpc,
		E,
		value => value,
		luci,
		{ location: { protocol: 'http:', hostname: '192.168.1.1' } },
		playDocument
	);
	responses['nes-emulator.access'] = {
		ok: false,
		error: 'nesd could not be started'
	};
	const playLoadOffset = calls.length;
	const failedAccess = await playComponent.load();
	const playLoadCall = calls.slice(playLoadOffset).find(call =>
		call.method === 'nes-emulator.access'
	);
	if (!playLoadCall || playLoadCall.rpcTimeout !== 120 ||
	    luci.env.rpctimeout !== 23) {
		throw new Error('Play access lacks a restored 120-second RPC timeout');
	}
	const failedPlay = JSON.stringify(playComponent.render(failedAccess));
	if (!failedPlay.includes('nesd could not be started') ||
	    !failedPlay.includes('Retry') ||
	    !failedPlay.includes('"data-nes-retry":"1"') ||
	    failedPlay.includes('"tag":"a"')) {
		throw new Error('Play startup failure did not expose a retry action');
	}

	const retryAccess = deferred();
	let retryAttempts = 0;
	responses['nes-emulator.access'] = () => {
		retryAttempts++;
		return retryAccess.promise;
	};
	const firstRetry = playComponent.onRetry();
	const duplicateRetry = playComponent.onRetry();
	if (retryAttempts !== 1)
		throw new Error('concurrent Play retries started nesd more than once');
	const retryCall = calls.filter(call =>
		call.method === 'nes-emulator.access'
	).at(-1);
	if (!retryCall || retryCall.rpcTimeout !== 120 ||
	    luci.env.rpctimeout !== 23) {
		throw new Error('Play retry did not preserve its extended RPC timeout');
	}
	const busyPlay = JSON.stringify(
		playDocument.getElementById('nes-play-access').children
	);
	if (!busyPlay.includes('Starting nesd') ||
	    !busyPlay.includes('"disabled":true')) {
		throw new Error('Play retry did not expose its busy state');
	}
	retryAccess.reject(new Error('nesd is still starting'));
	await Promise.all([ firstRetry, duplicateRetry ]);
	const failedRetry = JSON.stringify(
		playDocument.getElementById('nes-play-access').children
	);
	if (!failedRetry.includes('nesd is still starting') ||
	    !failedRetry.includes('Retry') ||
	    failedRetry.includes('"disabled":true')) {
		throw new Error('failed Play retry did not remain retryable');
	}

	responses['nes-emulator.access'] = () => {
		retryAttempts++;
		return {
			token: '0123456789abcdef0123456789abcdef'
		};
	};
	await playComponent.onRetry();
	if (retryAttempts !== 2)
		throw new Error('a subsequent Play retry did not call access again');
	const readyPlay = JSON.stringify(
		playDocument.getElementById('nes-play-access').children
	);
	if (!readyPlay.includes('Open game window') ||
	    !readyPlay.includes(
		    'http://192.168.1.1:29876/play#token=' +
		    '0123456789abcdef0123456789abcdef'
	    ) ||
	    !readyPlay.includes('"target":"_blank"') ||
	    !readyPlay.includes('"rel":"noopener noreferrer"') ||
	    readyPlay.includes('?token=') ||
	    readyPlay.includes('alert-message warning') ||
	    readyPlay.includes('data-nes-retry')) {
		throw new Error('successful Play retry did not expose the game client');
	}

	const httpsPlayComponent = playFactory(
		{ extend: value => value },
		rpc,
		E,
		value => value,
		luci,
		{ location: { protocol: 'https:', hostname: 'fd00::1' } },
		playDocument
	);
	const httpsPlay = JSON.stringify(httpsPlayComponent.render({
		token: 'fedcba9876543210fedcba9876543210',
		port: 29876
	}));
	if (!httpsPlay.includes(
		'http://[fd00::1]:29876/play#token=' +
		'fedcba9876543210fedcba9876543210'
	) ||
	    !httpsPlay.includes('"target":"_blank"') ||
	    !httpsPlay.includes('"rel":"noopener noreferrer"') ||
	    !httpsPlay.includes('"tag":"details"') ||
	    !httpsPlay.includes('Connection help') ||
	    !httpsPlay.includes(
		    'The game client opens in a separate local window'
	    ) ||
	    httpsPlay.includes('?token=') ||
	    httpsPlay.includes('alert-message warning')) {
		throw new Error(
			'healthy HTTPS Play view is unsafe or still looks like an error'
		);
	}

	const httpsOverviewComponent = factory(
		{ extend: value => value },
		rpc,
		{
			createHandlerFn: () => () => {},
			addNotification: () => {},
			uploadFile: async () => uploadedFile
		},
		E,
		value => value,
		luci,
		{ location: { protocol: 'https:', hostname: 'fd00::1' } },
		documentMock,
		() => 0
	);
	const httpsOverview = JSON.stringify(
		httpsOverviewComponent.render({ access: true })
	);
	if (!httpsOverview.includes('"tag":"details"') ||
	    !httpsOverview.includes('Connection help') ||
	    httpsOverview.includes('nesd serves plain HTTP on the LAN') ||
	    httpsOverview.includes('alert-message warning')) {
		throw new Error(
			'healthy HTTPS Overview still presents transport help as an error'
		);
	}

	process.stdout.write('LuCI RPC contract: OK\n');
})().catch(error => {
	console.error(error);
	process.exitCode = 1;
});
