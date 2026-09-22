#!/usr/bin/env python3
"""Assemble imagegen artwork and code-native UI. Requires ImageMagick, Node sharp and Nimbus Sans.
No image synthesis or artistic retouching is performed here.
"""
from pathlib import Path
import subprocess, shutil, json, hashlib, html

ROOT = Path(__file__).resolve().parents[1]
GENERATED = ROOT.parent / 'generated_images'
SOURCES = {
 'portrait': {'boot':'3bc98746-d07d-4495-95f3-64e62150e33d','lock':'df8b0757-762f-4eaf-bc61-f00c6e26cda2','open':'c05bf8d8-d1c8-4902-89b8-2044108abffc','launcher':'8fb678b7-0c83-432f-a8a9-7659759534f2'},
 'landscape': {'boot':'ce17c3ef-79c6-4b52-8919-544ed8ece685','lock':'e4a77fda-3ae7-4d1d-a920-2b087eea0e65','open':'e88b694c-c7a4-429b-a501-8291c5dbf714','launcher':'064081fb-20de-4aa0-a408-9f134b96e05e'},
}
FG='#eeeae2'; SECONDARY='#c3c0b9'; ACCENT='#e6d5b7'
manifest={'version':'1.0','kind':'visual asset handoff, not firmware','colors':{'foreground':FG,'secondary':SECONDARY,'focus':ACCENT,'background':'#101214'},'font':{'family':'Nimbus Sans','file':'fonts/NimbusSans-Regular.otf','runtime':'Prefer existing project font if available; verify text widths.'},'sample_data':{'clock':'17:24','date':'Monday, 21 September','battery_percent':86,'boot_progress':0.4,'selected_app':'Radio'},'orientations':{}}

def run(*args): subprocess.run([str(a) for a in args],check=True)
for orientation, sources in SOURCES.items():
    portrait=orientation=='portrait'; w,h=(568,1232) if portrait else (1232,568)
    spec={'width':w,'height':h,'safe_margin':28,'screens':{}}
    manifest['orientations'][orientation]=spec
    for group in ['backgrounds','screens','overlays','masters']:
        (ROOT/group/orientation).mkdir(parents=True,exist_ok=True)
    for screen,source_id in sources.items():
        master=ROOT/'masters'/orientation/(screen+'.png')
        if not master.exists(): shutil.copy2(GENERATED/('exec-'+source_id+'.png'),master)
        bg=ROOT/'backgrounds'/orientation/(screen+'.png')
        # Art was composed in matching orientation. Normalize only the tiny aspect rounding difference.
        run('convert',master,'-filter','Lanczos','-resize',f'{w}x{h}!','-colorspace','sRGB','-depth','8','-strip','PNG24:'+str(bg))
        parts=[]; elements=[]
        def rect(x,y,rw,rh,fill,opacity=1,rx=0):
            parts.append(f'<rect x="{x}" y="{y}" width="{rw}" height="{rh}" fill="{fill}" opacity="{opacity}" rx="{rx}"/>')
        def text(key,label,x,y,size,anchor='start',color=FG,spacing=0):
            parts.append(f'<text x="{x}" y="{y}" font-family="Nimbus Sans" font-size="{size}" fill="{color}" text-anchor="{anchor}" letter-spacing="{spacing}">{html.escape(label)}</text>')
            elements.append({'id':key,'sample':label,'x':x,'baseline_y':y,'font_px':size,'anchor':anchor,'color':color,'letter_spacing':spacing})
        # These lightweight UI scrims are intentionally separate from photographic artwork.
        rect(0,0,w,82,'#080a0d',0.26)
        if screen=='launcher': rect(0,0,w,h,'#080a0d',0.20)
        text('brand','DOORS',28,46,17,spacing=3)
        if screen=='boot':
            text('hardware','K230',w-28,46,14,'end',SECONDARY,2)
        else:
            # Static examples only. Runtime supplies real connectivity and charge.
            x=w-132
            parts.append(f'<g fill="none" stroke="{FG}" stroke-width="1.7" stroke-linecap="round"><path d="M{x} 34 Q{x+9} 26 {x+18} 34 M{x+4} 39 Q{x+9} 34 {x+14} 39"/><circle cx="{x+9}" cy="43" r="1" fill="{FG}"/><rect x="{w-102}" y="32" width="24" height="13" rx="2"/><path d="M{w-76} 36 v5"/></g>')
            rect(w-99,35,16,7,FG)
            text('battery','86%',w-28,45,16,'end')
        if screen=='boot':
            text('wordmark','DOORS',w/2,530 if portrait else 248,52 if portrait else 62,'middle',FG,8)
            text('tagline','Open. Explore. Connect.',w/2,574 if portrait else 289,20,'middle',ACCENT,0.7)
            py=1015 if portrait else 452
            text('boot_status','Starting system',w/2,py-22,17,'middle',SECONDARY)
            rect(w/2-135,py,270,4,'#777b7e',0.55,2)
            rect(w/2-135,py,108,4,ACCENT,1,2)
        elif screen=='lock':
            text('clock','17:24',w/2,294 if portrait else 210,98 if portrait else 108,'middle')
            text('date','Monday, 21 September',w/2,336 if portrait else 253,21,'middle',SECONDARY)
            py=1055 if portrait else 445
            parts.append(f'<g transform="translate({w/2-10},{py})" fill="none" stroke="{FG}" stroke-width="1.5"><rect y="10" width="20" height="18" rx="2"/><path d="M4 10 V5 A6 6 0 0 1 16 5 V10 M10 17 v5"/></g>')
            text('unlock_hint','Tap to open',w/2,py+58,18,'middle',SECONDARY)
        elif screen=='open':
            if portrait:
                text('clock','17:24',w/2,127,42,'middle')
            else: text('clock','17:24',w/2,46,28,'middle')
            rect(0,h-70,w,70,'#080a0d',0.28)
            text('tagline','Open. Explore. Connect.',w/2,h-31,17,'middle',FG,0.5)
        else:
            text('clock','17:24',w/2,244 if portrait else 91,68 if portrait else 50,'middle')
            text('date','Monday, 21 September',w/2,283 if portrait else 123,20,'middle',SECONDARY)
            apps=['Radio','Mesh','Network','Tools','AI','Games','Settings','Files','Apps']
            startx=62 if portrait else 302; dx=150 if portrait else 232
            firsty=491 if portrait else 219; dy=174 if portrait else 108
            cellw=144 if portrait else 216
            cells=[]
            for i,name in enumerate(apps):
                x=startx+(i%3)*dx; y=firsty+(i//3)*dy
                if i==0:
                    rect(x-14,y-40,cellw,68,ACCENT,0.09)
                    rect(x-14,y-40,3,68,ACCENT)
                text('app_'+name.lower(),name,x,y,25 if portrait else 33)
                rect(x,y+40,cellw-24,1,SECONDARY,0.24)
                cells.append({'app_id':name.lower(),'label':name,'x':x-14,'y':y-40,'width':cellw,'height':100,'selected':i==0})
            text('tagline','Open. Explore. Connect.',w/2 if portrait else 28,h-76 if portrait else h-28,16,'middle' if portrait else 'start',SECONDARY,0.4)
            text('navigation_hint','Tap or Enter to open',w/2 if portrait else w-28,h-38 if portrait else h-28,15,'middle' if portrait else 'end',SECONDARY)
        svg=f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">'+''.join(parts)+'</svg>'
        overlay=ROOT/'overlays'/orientation/(screen+'.svg'); overlay.write_text(svg)
        overlaypng=overlay.with_suffix('.png')
        run('node','-e',"require('sharp')(process.argv[1]).png().toFile(process.argv[2]).catch(e=>{console.error(e);process.exit(1)})",overlay,overlaypng)
        output=ROOT/'screens'/orientation/(screen+'.png')
        run('convert',bg,overlaypng,'-compose','over','-composite','-depth','8','PNG24:'+str(output))
        spec['screens'][screen]={'background':str(bg.relative_to(ROOT)),'preview':str(output.relative_to(ROOT)),'overlay_svg':str(overlay.relative_to(ROOT)),'overlay_png':str(overlaypng.relative_to(ROOT)),'text':elements}
        if screen=='launcher': spec['screens'][screen]['cells']=cells
        if screen=='boot': spec['screens'][screen]['progress']={'x':w/2-135,'y':py,'width':270,'height':4,'sample_fraction':0.4}
(ROOT/'ui_layout.json').write_text(json.dumps(manifest,indent=2)+'\n')
(ROOT/'fonts').mkdir(exist_ok=True)
shutil.copy2('/usr/share/fonts/opentype/urw-base35/NimbusSans-Regular.otf',ROOT/'fonts/NimbusSans-Regular.otf')
shutil.copy2('/usr/share/doc/fonts-urw-base35/copyright',ROOT/'fonts/LICENSE.txt')
# Contact sheets are presentation artifacts, assembled from native screen exports.
run('montage',*[ROOT/'screens/portrait'/(s+'.png') for s in ['boot','lock','open','launcher']],'-tile','4x1','-geometry','284x616+12+12','-background','#101214',ROOT/'Portrait_overview.png')
run('montage',*[ROOT/'screens/landscape'/(s+'.png') for s in ['boot','lock','open','launcher']],'-tile','2x2','-geometry','616x284+12+12','-background','#101214',ROOT/'Landscape_overview.png')
print('Built 8 clean backgrounds, 8 screen previews, 8 SVG + 8 PNG overlays, 8 masters and 2 contact sheets.')
