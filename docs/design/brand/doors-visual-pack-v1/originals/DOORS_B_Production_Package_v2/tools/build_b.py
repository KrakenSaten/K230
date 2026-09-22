from pathlib import Path
import shutil, subprocess, json, html
R=Path(__file__).resolve().parents[1]
V=R.parent/'DOORS_Visual_Package_v1'
def run(*a):subprocess.run([str(x) for x in a],check=True)
def render(src,dst,size=None):
 code="let s=require('sharp')(process.argv[1]); if(process.argv[3])s=s.resize(+process.argv[3],+process.argv[3]);s.png().toFile(process.argv[2]).catch(e=>{console.error(e);process.exit(1)})"
 run('node','-e',code,src,dst,*([size] if size else []))
if not (R/'backgrounds').exists():
 for d in ['backgrounds','fonts','screens','overlays']:shutil.copytree(V/d,R/d)
shutil.copy2(V/'ui_layout.json',R/'base_ui_layout.json') if V.exists() else None
for d in ['icons/svg','icons/png48','icons/png64','icons/png96','icons/png128','glyphs/svg','glyphs/png32','references']:(R/d).mkdir(parents=True,exist_ok=True)
reference=R.parent/'generated_images/exec-52980e5e-1b16-41c0-9baa-a278891272e9.png'
if reference.exists():shutil.copy2(reference,R/'references/approved_B.png')
colors={'radio':'#b5cfa5','mesh':'#a6c4da','network':'#9fbdd5','tools':'#dcb387','ai':'#b9afd4','games':'#dfad85','settings':'#d5d7d5','files':'#d7b78a','apps':'#e5e2d4'}
paths={
'radio':'<path d="M18 27 L24 9 L30 27 M21 20 H27 M15 7 Q9 14 15 21 M33 7 Q39 14 33 21 M10 3 Q0 14 10 25 M38 3 Q48 14 38 25"/><circle cx="24" cy="7" r="2"/>',
'mesh':'<circle cx="24" cy="5" r="5"/><circle cx="8" cy="28" r="5"/><circle cx="40" cy="28" r="5"/><path d="M20 9 L11 23 M28 9 L37 23 M13 28 H35"/>',
'network':'<circle cx="24" cy="17" r="16"/><ellipse cx="24" cy="17" rx="7" ry="16"/><path d="M8 17 H40 M11 9 H37 M11 25 H37"/>',
'tools':'<path d="M29 3 Q19 1 18 12 L5 25 Q1 29 5 32 Q8 35 12 31 L25 18 Q36 18 36 7 L29 12 L24 8 Z"/>',
'ai':'<rect x="12" y="5" width="24" height="24" rx="2"/><path d="M17 0 V5 M24 0 V5 M31 0 V5 M17 29 V34 M24 29 V34 M31 29 V34 M7 10 H12 M7 17 H12 M7 24 H12 M36 10 H41 M36 17 H41 M36 24 H41"/>',
'games':'<path d="M13 8 H35 Q40 8 42 15 L46 29 Q46 35 40 31 L32 25 H16 L8 31 Q2 35 2 29 L6 15 Q8 8 13 8 Z M14 13 V23 M9 18 H19"/><circle cx="33" cy="15" r="1"/><circle cx="38" cy="20" r="1"/>',
'settings':'<path d="M20 1 H28 L29 6 L34 8 L39 6 L44 13 L40 17 L40 21 L44 25 L39 32 L34 30 L29 32 L28 37 H20 L19 32 L14 30 L9 32 L4 25 L8 21 L8 17 L4 13 L9 6 L14 8 L19 6 Z"/><circle cx="24" cy="19" r="7"/>',
'files':'<path d="M4 29 V5 Q4 2 7 2 H19 L24 8 H41 Q44 8 44 11 V29 Q44 32 41 32 H7 Q4 32 4 29 Z"/>',
'apps':'<rect x="5" y="1" width="15" height="13" rx="2"/><rect x="28" y="1" width="15" height="13" rx="2"/><rect x="5" y="22" width="15" height="13" rx="2"/><rect x="28" y="22" width="15" height="13" rx="2"/>',
'wifi':'<path d="M3 9 Q24 -8 45 9 M10 17 Q24 4 38 17 M17 25 Q24 18 31 25"/><circle cx="24" cy="32" r="1.5"/>',
'bluetooth':'<path d="M22 1 V35 L36 25 L10 7 M10 29 L36 11 L22 1"/>',
'sound':'<path d="M4 13 H13 L23 4 V32 L13 23 H4 Z M29 11 Q36 18 29 25 M35 5 Q47 18 35 31"/>',
'display':'<rect x="3" y="2" width="42" height="26" rx="2"/><path d="M24 28 V35 M15 35 H33"/>',
'sun':'<circle cx="24" cy="18" r="8"/><path d="M24 0 V5 M24 31 V36 M6 18 H11 M37 18 H42 M11 5 L15 9 M33 27 L37 31 M11 31 L15 27 M33 9 L37 5"/>',
'lock':'<rect x="10" y="15" width="28" height="21" rx="2"/><path d="M16 15 V8 A8 8 0 0 1 32 8 V15 M24 23 V29"/>',
'power':'<path d="M24 1 V18 M13 6 A16 16 0 1 0 35 6"/>',
'info':'<circle cx="24" cy="18" r="16"/><path d="M24 16 V28"/><circle cx="24" cy="9" r="1"/>'}
def glyph(name,x=0,y=0,size=32,color='#eeeae2'):
 return f'<g transform="translate({x},{y}) scale({size/48})" fill="none" stroke="{color}" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">{paths[name]}</g>'
def portal(name):
 c=colors[name]
 return f'''<defs><linearGradient id="outer" x2="1" y2="1"><stop stop-color="#8b8e88"/><stop offset=".36" stop-color="#363b3c"/><stop offset="1" stop-color="#171b1d"/></linearGradient><linearGradient id="inner" x2="0" y2="1"><stop stop-color="#0d1115"/><stop offset="1" stop-color="#242a2d"/></linearGradient></defs><path d="M22 9 H106 V119 H22 Z" fill="url(#outer)" stroke="#707671" stroke-width="1"/><path d="M32 20 H96 V110 H32 Z" fill="url(#inner)" stroke="#07090b" stroke-width="2"/><path d="M22 9 L32 20 M106 9 L96 20 M22 119 L32 110 M106 119 L96 110" stroke="#a2a59c" stroke-opacity=".42"/><path d="M32 20 L38 25 V105 L32 110" fill="#343b3d"/><path d="M37 25 V104" stroke="{c}" stroke-opacity=".4"/>{glyph(name,40,46,48,c)}'''
for name in paths:
 svg=f'<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48" viewBox="0 -6 48 48">{glyph(name,0,0,48)}</svg>'
 p=R/'glyphs/svg'/f'{name}.svg';p.write_text(svg);render(p,R/'glyphs/png32'/f'{name}.png',32)
for name in colors:
 p=R/'icons/svg'/f'{name}.svg';p.write_text(f'<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128">{portal(name)}</svg>')
 for size in [48,64,96,128]:render(p,R/f'icons/png{size}'/f'{name}.png',size)
layout={'version':'2.0-B','coordinates':'native pixels; text y is baseline','colors':colors,'groups':[{'name':'Connections','apps':['radio','mesh','network']},{'name':'Workspace','apps':['tools','ai','files']},{'name':'Device & play','apps':['games','settings','apps']}],'orientations':{}}
for orient,w,h in [('portrait',568,1232),('landscape',1232,568)]:
 portrait=orient=='portrait';layout['orientations'][orient]={'width':w,'height':h,'screens':{}}
 for screen in ['launcher','system']:
  parts=[];texts=[];controls=[]
  def rect(x,y,ww,hh,fill='#161c20',opacity=.56,stroke=None):parts.append(f'<rect x="{x}" y="{y}" width="{ww}" height="{hh}" rx="4" fill="{fill}" fill-opacity="{opacity}"'+(f' stroke="{stroke}" stroke-opacity=".42"' if stroke else '')+'/>')
  def text(key,t,x,y,size=20,anchor='start',color='#eeeae2',spacing=0):
   parts.append(f'<text x="{x}" y="{y}" font-family="Nimbus Sans" font-size="{size}" fill="{color}" text-anchor="{anchor}" letter-spacing="{spacing}">{html.escape(t)}</text>');texts.append({'id':key,'sample':t,'x':x,'baseline_y':y,'font_px':size,'anchor':anchor})
  def line(x,y,x2,y2,color='#929892',opacity=.4):parts.append(f'<path d="M{x} {y} L{x2} {y2}" fill="none" stroke="{color}" stroke-opacity="{opacity}"/>')
  def control(key,x,y,ww,hh,role):controls.append({'id':key,'x':x,'y':y,'width':ww,'height':hh,'role':role})
  rect(0,0,w,h,'#0a1014',.24)
  text('brand','DOORS',28,43,17,spacing=3)
  parts.append(glyph('wifi',w-139,25,24));parts.append(f'<rect x="{w-103}" y="29" width="24" height="12" rx="2" fill="none" stroke="#eeeae2"/><rect x="{w-100}" y="32" width="17" height="6" fill="#eeeae2"/>')
  text('battery','86%',w-28,41,16,'end')
  if screen=='launcher':
   text('clock','17:24',w/2,225 if portrait else 79,76 if portrait else 46,'middle')
   text('date','Monday, 21 September',w/2,267 if portrait else 111,21 if portrait else 18,'middle', '#c3c0b9')
   for gi,group in enumerate(layout['groups']):
    gx,gy,gw,gh=(36,336+gi*235,496,214) if portrait else (36+gi*392,152,376,305)
    rect(gx,gy,gw,gh,stroke='#a0a8a4');text(f'group{gi}',group['name'].upper(),gx+18,gy+29,15,spacing=1.5);line(gx,gy+44,gx+gw,gy+44)
    for ai,name in enumerate(group['apps']):
     icon_size=104 if portrait else 100; cx=gx+gw*(ai+.5)/3;iy=gy+62 if portrait else gy+89
     # Namespace SVG gradient ids for safe embedding of all 9 portals.
     content=portal(name).replace('id="outer"',f'id="outer_{name}"').replace('id="inner"',f'id="inner_{name}"').replace('url(#outer)',f'url(#outer_{name})').replace('url(#inner)',f'url(#inner_{name})')
     parts.append(f'<g transform="translate({cx-icon_size/2},{iy}) scale({icon_size/128})">{content}</g>')
     label={'ai':'AI'}.get(name,name.title());text(name,label,cx,iy+icon_size+24,20,'middle')
     control('app_'+name,cx-gw/6+6,gy+50,gw/3-12,gh-58,'app')
     if name=='radio':
      sx=cx-icon_size/2-4;sy=iy-3
      parts.append(f'<path d="M{sx} {sy+17} V{sy} H{sx+18} M{sx+icon_size+8-18} {sy} H{sx+icon_size+8} V{sy+17}" fill="none" stroke="#b5cfa5" stroke-width="2"/>')
      line(cx-48,iy+icon_size+36,cx+48,iy+icon_size+36,'#b5cfa5',1)
   text('hint','Open a space',w/2,h-55,18,'middle','#c3c0b9',.5)
  else:
   text('title','System',36,151 if portrait else 102,46 if portrait else 38)
   text('subtitle','Quick controls',36,186 if portrait else 130,21,'start','#c3c0b9')
   tx,ty,tw,th,gap=(36,250,240,120,16) if portrait else (36,160,252,115,16)
   for i,(name,label,state,on) in enumerate([('wifi','Wi-Fi','Connected',True),('bluetooth','Bluetooth','Off',False),('radio','Radio','Ready',True),('sound','Sound','On',True)]):
    x=tx+(i%2)*(tw+gap);y=ty+(i//2)*(th+gap);rect(x,y,tw,th,stroke='#a0a8a4');parts.append(glyph(name,x+20,y+36,42));text(name,label,x+78,y+49,21);text(name+'_state',state,x+78,y+78,17,color='#c3c0b9');parts.append(f'<circle cx="{x+tw-18}" cy="{y+20}" r="4" fill="'+('#b5cfa5' if on else '#7f8586')+'"/>');control(name,x,y,tw,th,'quick_control')
   sx,sy,sw,sh=(36,538,496,219) if portrait else (594,160,602,139)
   rect(sx,sy,sw,sh,stroke='#a0a8a4')
   for i,(name,label,value) in enumerate([('sun','Brightness',60),('sound','Volume',40)]):
    y=sy+24+i*(104 if portrait else 65);parts.append(glyph(name,sx+18,y+3,28));text(name+'_label',label,sx+62,y+21,19);text(name+'_value',str(value)+'%',sx+sw-22,y+21,18,'end');ly=y+49 if portrait else y+37;lx=sx+62;lw=sw-90;line(lx,ly,lx+lw,ly,'#9a9e98',.65);parts.append(f'<path d="M{lx} {ly} H{lx+lw*value/100}" stroke="#e5e2d4" stroke-width="4"/><circle cx="{lx+lw*value/100}" cy="{ly}" r="7" fill="#e5e2d4"/>');control(label.lower(),lx-10,ly-16,lw+20,32,'slider')
   rx,ry,rw,rh=(36,782,496,72) if portrait else (594,316,602,55)
   rect(rx,ry,rw,rh*3,stroke='#a0a8a4')
   for i,(name,label) in enumerate([('display','Display & sleep'),('network','Connections'),('info','About DOORS')]):
    y=ry+i*rh;parts.append(glyph(name,rx+19,y+(rh-26)/2,32));text(name+'_row',label,rx+68,y+rh/2+7,21);parts.append(f'<path d="M{rx+rw-29} {y+rh/2-6} l6 6 l-6 6" fill="none" stroke="#eeeae2" stroke-width="1.5"/>');control(name+'_details',rx,y,rw,rh,'navigate');line(rx,y+rh,rx+rw,y+rh)
   ax,ay,aw,ah=(36,1030,240,77) if portrait else (36,444,252,64)
   for i,name in enumerate(['lock','power']):
    x=ax+i*(aw+16);rect(x,ay,aw,ah,stroke='#a0a8a4');parts.append(glyph(name,x+22,ay+(ah-27)/2,32));text(name+'_action',name.title(),x+75,ay+ah/2+7,22);control(name,x,ay,aw,ah,'action')
  svg=f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">'+''.join(parts)+'</svg>'
  p=R/'overlays'/orient/(screen+'.svg');p.write_text(svg);render(p,p.with_suffix('.png'))
  bg=R/'backgrounds'/orient/'launcher.png'
  if screen=='system':shutil.copy2(bg,R/'backgrounds'/orient/'system.png')
  run('convert',bg,p.with_suffix('.png'),'-compose','over','-composite','-depth','8','PNG24:'+str(R/'screens'/orient/(screen+'.png')))
  layout['orientations'][orient]['screens'][screen]={'background':f'backgrounds/{orient}/{screen}.png','preview':f'screens/{orient}/{screen}.png','overlay':f'overlays/{orient}/{screen}.svg','text':texts,'controls':controls}
(R/'b_ui_layout.json').write_text(json.dumps(layout,indent=2)+'\n')
run('montage',*[R/'screens/portrait'/(s+'.png') for s in ['launcher','system']],'-tile','2x1','-geometry','284x616+12+12','-background','#101214',R/'B_Portrait_overview.png')
run('montage',*[R/'screens/landscape'/(s+'.png') for s in ['launcher','system']],'-tile','1x2','-geometry','924x426+12+12','-background','#101214',R/'B_Landscape_overview.png')
run('montage',*[R/'icons/png128'/(s+'.png') for s in colors],'-tile','9x1','-geometry','128x128+8+8','-background','#101214',R/'B_Icon_family.png')
print('B assets complete')
