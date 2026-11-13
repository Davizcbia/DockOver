// Executes the lab's real export/import functions without a GUI or user settings.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const source = fs.readFileSync(path.join(__dirname, '../Lab.qml'), 'utf8');
const options = JSON.parse(source.match(/readonly property var advancedOptions: (.*)/)[1]);
const sliderOptions = JSON.parse(source.match(/readonly property var sliderOptions: (.*)/)[1]);
const ctx = { advancedOptions: options, sliderOptions };
ctx.lab = ctx;
vm.createContext(ctx);
vm.runInContext(source.slice(source.indexOf('    function parameters()'), source.indexOf('    Settings {')), ctx);
ctx.apply({});
const baseline = JSON.stringify(ctx.parameters());
assert.equal(ctx.parameters().version, 3);
assert.equal(options.length, 27);
assert.equal(ctx.frost, 0.3);
assert.equal(ctx.lens, 1);
assert.equal(ctx.reflection, 0.4);
assert.equal(ctx.staticReflection, true);
assert.equal(ctx.sidebarFrost, 1);
assert.equal(sliderOptions.length, 9);
for (const option of [...options, ...sliderOptions]) {
    assert.ok(!option.label.includes('?'), `Unreadable label: ${option.key}`);
    ctx.apply({[option.key]:option.max});
    assert.equal(ctx[option.key],option.max);
    const saved = JSON.parse(JSON.stringify(ctx.parameters()));
    ctx.apply({});
    ctx.apply(saved);
    assert.equal(ctx[option.key],option.max);
    ctx.apply({[option.key]:option.min-100});
    assert.equal(ctx[option.key],option.min);
}
ctx.apply({frost:0.2,refraction:1,density:0.1,radius:45,dark:false});
assert.equal(ctx.frost,0.2);
assert.equal(ctx.refractiveIndex,1.45); // old parameter payloads remain usable
assert.equal(ctx.darkGlass,false);
ctx.apply({materialVariant:2,reduceBrightEffects:true,increaseContrast:true,sliderCustom:true,sliderRefraction:0.42});
const material = JSON.parse(JSON.stringify(ctx.parameters()));
ctx.apply({});
assert.equal(ctx.materialVariant,0); // old payloads preserve the original appearance
ctx.apply(material);
assert.equal(ctx.materialVariant,2);
assert.equal(ctx.reduceBrightEffects,true);
assert.equal(ctx.increaseContrast,true);
assert.equal(ctx.sliderCustom,true);
assert.equal(ctx.sliderRefraction,0.42);
for (const invalid of [null, [], {frost:'bad'}, {dispersion:NaN}, {lightAngle:Infinity}, {dark:'false'}, {materialVariant:1.5}, {reduceBrightEffects:'yes'}, {staticReflection:'yes'}, {sidebarGlow:NaN}, {sliderCustom:'yes'}, {sliderGain:NaN}]) {
    const before = JSON.stringify(ctx.parameters());
    assert.throws(()=>ctx.apply(invalid));
    assert.equal(JSON.stringify(ctx.parameters()),before); // no partial updates
}
const variant = JSON.stringify(ctx.parameters());
ctx.apply(JSON.parse(baseline));
assert.equal(JSON.stringify(ctx.parameters()),baseline);
ctx.apply(JSON.parse(variant));
assert.equal(JSON.stringify(ctx.parameters()),variant);
console.log('Global and independent slider controls: round-trip, limits, legacy import, atomic rejection and A/B restore passed');
