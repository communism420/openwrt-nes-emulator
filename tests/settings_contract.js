'use strict';

const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
const source = fs.readFileSync(path.join(
	root,
	'package/luci-app-nes-emulator/htdocs/luci-static/resources/view/' +
		'nes-emulator/settings.js'
), 'utf8');

const renderedOptions = [];

class MockOption {
	constructor(type, name, title, description) {
		this.type = type;
		this.name = name;
		this.title = title;
		this.description = description;
		this.disabled = '0';
		this.enabled = '1';
		this.dependencies = [];
		this.choices = [];
	}

	depends(...constraint) {
		this.dependencies.push(constraint);
		return this;
	}

	value(...choice) {
		this.choices.push(choice);
		return this;
	}
}

class MockSection {
	option(type, name, title, description) {
		const option = new MockOption(type, name, title, description);
		renderedOptions.push(option);
		return option;
	}
}

class MockMap {
	section() {
		return new MockSection();
	}

	render() {
		return this;
	}
}

const form = {
	Map: MockMap,
	TypedSection: Symbol('TypedSection'),
	Flag: Symbol('Flag'),
	DummyValue: Symbol('DummyValue'),
	Value: Symbol('Value'),
	DynamicList: Symbol('DynamicList'),
	Button: Symbol('Button'),
	ListValue: Symbol('ListValue')
};

const factory = new Function(
	'view', 'form', 'rpc', 'ui', 'E', '_',
	source
);
const component = factory(
	{ extend: value => value },
	form,
	{ declare: () => async () => ({}) },
	{},
	() => ({}),
	value => value
);

component.render();

function option(name) {
	const found = renderedOptions.find(candidate => candidate.name === name);
	if (!found)
		throw new Error(`settings option ${name} was not rendered`);
	return found;
}

const gate = option('extra_rom_dirs_enabled');
if (gate.type !== form.Flag || gate.default !== '0' || gate.rmempty !== false)
	throw new Error('extra ROM directory gate is not an explicit default-off flag');

const extras = option('extra_rom_dir');
if (extras.type !== form.DynamicList ||
    extras.retain !== true ||
    extras.dependencies.length !== 1 ||
    extras.dependencies[0][0] !== 'extra_rom_dirs_enabled' ||
    extras.dependencies[0][1] !== '1') {
	throw new Error('extra ROM paths are not retained behind the enable gate');
}
if (extras.validate('main', '') !== true ||
    extras.validate('main', undefined) !== true) {
	throw new Error('an empty optional extra ROM entry still blocks saving');
}
if (extras.validate('main', '/mnt/sda1/roms') !== true)
	throw new Error('a safe absolute extra ROM directory was rejected');
if (extras.validate('main', 'mnt/sda1/roms') === true ||
    extras.validate('main', '/mnt') === true ||
    extras.validate('main', '/mnt/sda1:roms') === true ||
    extras.validate('main', '/mnt/sda1;roms') === true ||
    extras.validate('main', '/mnt/sda1,roms') === true) {
	throw new Error('an unsafe extra ROM directory was accepted');
}

const streamFps = option('stream_fps');
if (streamFps.type !== form.Value ||
    streamFps.datatype !== 'range(1,60)' ||
    streamFps.default !== '2' ||
    streamFps.rmempty !== false) {
	throw new Error('stream FPS is not an explicit 1..60 value with a safe default');
}
if (!streamFps.description.includes('1–60 FPS') ||
    !streamFps.description.includes('native 50/60 FPS timing')) {
	throw new Error('stream FPS help does not distinguish delivery from emulation timing');
}

const showFps = option('show_fps');
if (showFps.type !== form.Flag ||
    showFps.default !== '1' ||
    showFps.rmempty !== false) {
	throw new Error('FPS counter is not an explicit default-on flag');
}
const expectedShowFpsDescription =
	'Draw only the browser-painted FPS number as a FCEUX-like pixel OSD ' +
	'directly over the NES canvas in its top-right corner, without a separate ' +
	'browser widget. The number reflects delivery, decode and paint slowdowns ' +
	'rather than the ROM’s native frame rate or the configured stream limit. ' +
	'The setting is applied after Save & Apply and the game window reconnects.';
if (showFps.description !== expectedShowFpsDescription) {
	throw new Error('FPS counter help differs from the reviewed LuCI wording');
}
if (!showFps.description.includes('browser-painted FPS number') ||
    !showFps.description.includes('FCEUX-like pixel OSD') ||
    !showFps.description.includes('directly over the NES canvas') ||
    !showFps.description.includes('without a separate browser widget') ||
    !showFps.description.includes('delivery, decode and paint slowdowns') ||
    !showFps.description.includes('configured stream limit')) {
	throw new Error('FPS counter help does not define numeric canvas-OSD semantics');
}

const showTouchControls = option('show_touch_controls');
if (showTouchControls.type !== form.Flag ||
    showTouchControls.default !== '1' ||
    showTouchControls.rmempty !== false) {
	throw new Error('on-screen controls are not an explicit default-on flag');
}
if (!showTouchControls.description.includes('free screen space') ||
    !showTouchControls.description.includes('keyboard and gamepad controls remain available') ||
    !showTouchControls.description.includes('Save & Apply')) {
	throw new Error('on-screen controls help does not explain desktop behavior or application timing');
}

const primary = option('rom_dir');
if (primary.validate('main', '') === true)
	throw new Error('the required primary ROM directory became optional');

console.log('settings contract: OK');
