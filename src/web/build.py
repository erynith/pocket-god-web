import base64, sys, os
d = os.path.dirname(os.path.abspath(__file__))
page = open(os.path.join(d, 'page.html')).read()
host = open(os.path.join(d, 'host.js')).read()
wasm = base64.b64encode(open(os.path.join(d, 'pgvm.wasm'), 'rb').read()).decode()
assert '</script' not in host
out = page.replace('/*__HOST_JS__*/', host).replace('__WASM_B64__', wasm)
open(os.path.join(d, 'dalvik-web-player.html'), 'w').write(out)
print('wrote', len(out), 'bytes')

skeleton = ('<!doctype html><html lang="en"><head><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">'
            '<style>:root{color-scheme:light;padding-top:env(safe-area-inset-top,0px);padding-bottom:env(safe-area-inset-bottom,0px)}'
            'body{margin:0;font:14px system-ui,sans-serif}img{max-width:100%}[hidden]{display:none!important}</style>'
            '</head><body>')
os.makedirs(os.path.join(d, 'site'), exist_ok=True)
open(os.path.join(d, 'site', 'index.html'), 'w').write(skeleton + out + '</body></html>')
print('wrote site/index.html')
