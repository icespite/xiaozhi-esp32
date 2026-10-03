import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

// Run the actual generated page scripts with a small DOM/fetch harness.
const html = readFileSync(process.argv[2], 'utf8');
const scripts = [...html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/g)].map(match => match[1]);
assert.equal(scripts.length, 2);
const event = {preventDefault() {}};

function page({getFails = false, saveFails = false, uploadGetFails = false, uploadSaveFails = false} = {}) {
    const nodes = new Map();
    const calls = [];
    let storedUrl = 'https://existing.example/news';
    let uploadUrl = 'http://192.168.1.20:8000/display';
    const node = id => {
        if (!nodes.has(id)) nodes.set(id, {
            value: '', textContent: '', disabled: id === 'web_url' || id === 'upload_url',
            classList: {add() {}, remove() {}}, addEventListener() {}
        });
        return nodes.get(id);
    };
    const context = vm.createContext({
        document: {getElementById: node, addEventListener() {}},
        window: {addEventListener() {}, location: {href: ''}},
        fetch: async (path, options = {}) => {
            calls.push({path, ...options});
            if (path === '/web/config') {
                const failed = options.method === 'POST' ? saveFails : getFails;
                if (failed) return {ok: false, text: async () => 'Save failed'};
                if (options.method === 'POST') storedUrl = options.body || 'https://icespite.top/';
                return {ok: true, json: async () => ({url: storedUrl})};
            }
            if (path === '/upload/config') {
                const failed = options.method === 'POST' ? uploadSaveFails : uploadGetFails;
                if (failed) return {ok: false, text: async () => 'Upload URL save failed'};
                if (options.method === 'POST') uploadUrl = options.body;
                return {ok: true, json: async () => ({url: uploadUrl})};
            }
            assert.equal(path, '/submit');
            assert.deepEqual(JSON.parse(options.body), {ssid: 'Home WiFi', password: 'password'});
            return {ok: true, json: async () => ({success: true})};
        }
    });
    for (const script of scripts) vm.runInContext(script, context);
    node('ssid').value = 'Home WiFi';
    node('password').value = 'password';
    return {context, node, calls};
}

// Existing configuration is shown; a new URL is saved before Wi-Fi submission.
{
    const {context, node, calls} = page();
    await context.loadWebUrl();
    assert.equal(node('web_url').value, 'https://existing.example/news');
    assert.equal(node('web_url').disabled, false);
    node('web_url').value = ' https://new.example/article?q=1&lang=zh ';
    await context.submitForm(event);
    assert.deepEqual(calls.map(call => [call.path, call.method || 'GET']), [
        ['/web/config', 'GET'], ['/web/config', 'POST'],
        ['/upload/config', 'GET'], ['/upload/config', 'POST'], ['/submit', 'POST']
    ]);
    assert.equal(calls[1].body, 'https://new.example/article?q=1&lang=zh');
    assert.equal(context.window.location.href, '/done.html');
}

// Saving separately restores defaults without connecting/restarting Wi-Fi.
{
    const {context, node, calls} = page();
    await context.loadWebUrl();
    node('web_url').value = '';
    await context.submitWebUrl(event);
    assert.equal(node('web_url').value, 'https://icespite.top/');
    assert.equal(node('web_url_save').disabled, false);
    assert(!calls.some(call => call.path === '/submit'));
}

// An initial GET failure must not erase an existing URL or proceed to Wi-Fi.
{
    const {context, node, calls} = page({getFails: true});
    await context.submitForm(event);
    assert(calls.every(call => call.method !== 'POST'));
    assert(node('error').textContent.includes('读取网址失败'));
    assert.equal(node('button').disabled, false);
}

// Failed persistence blocks the connect/redirect and permits retry.
{
    const {context, node, calls} = page({saveFails: true});
    await context.loadWebUrl();
    await context.submitForm(event);
    assert(!calls.some(call => call.path === '/submit'));
    assert.equal(node('error').textContent, 'Save failed');
    assert.equal(node('button').disabled, false);
    assert.equal(context.window.location.href, '');
}

// Unsupported protocols are caught without sending a write.
{
    const {context, node, calls} = page();
    await context.loadWebUrl();
    node('web_url').value = 'ftp://example.com/';
    await context.submitForm(event);
    assert(calls.every(call => call.method !== 'POST'));
    assert(node('error').textContent.includes('HTTP(S)'));
}

// Clicking Connect before the initial GET completes retains the old URL.
{
    const {context, calls} = page();
    await context.submitForm(event);
    assert.equal(calls[1].body, 'https://existing.example/news');
}
// The upload reader uses a separate URL and can be disabled without changing web.
{
    const {context, node, calls} = page();
    await context.loadUploadUrl();
    assert.equal(node('upload_url').value, 'http://192.168.1.20:8000/display');
    node('upload_url').value = '';
    await context.submitUploadUrl(event);
    assert.equal(node('upload_url').value, '');
    assert(calls.every(call => call.path === '/upload/config'));
}
for (const flags of [{uploadGetFails: true}, {uploadSaveFails: true}]) {
    const {context, node, calls} = page(flags);
    await context.submitForm(event);
    assert(!calls.some(call => call.path === '/submit'));
    assert(node('error').textContent);
    assert.equal(node('button').disabled, false);
}
console.log('Wi-Fi URL form integration tests passed (9 scenarios)');
