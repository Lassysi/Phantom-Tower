"""Static venue extraction from complete cooked CUE4Parse JSON exports.

Never opens a process or pipe. Default inputs are the existing bossmap exports,
plus build/tower-venues/maps and notes/dump/tower-growth map supplements.
Use --dump repeatedly to relocate the map inputs. --check regenerates in memory
and compares BOTH deliverables without writing them. Export indices are joins;
actor soft paths may cross cooked cells only on an unambiguous world/subpath.
"""
from __future__ import annotations
import argparse
from collections import defaultdict, Counter
from functools import lru_cache
import hashlib
import json
import math
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[3]
DATA = ROOT / 'mods/tower/data/venues.json'
REPORT = ROOT / 'mods/tower/data/venues-summary.md'
COVERAGE = ROOT / 'mods/tower/data/boss_live_coverage_32ffb7d.json'
LINKS = ROOT / 'mods/tower/data/native_arena_links.json'
JS = ROOT / 'notes/dump/tower-growth/ProjectPBZ/Content/JavaScript/Gameplay/AI/MobGroupScript_LinkFogDoor.js'
EVENTS = ['FogDoorsToActivate', 'FogDoorsToDeactivateOnClear',
          'FogDoorsToDeactivateOnOffCombat', 'FogDoorsToDeactivatePlayerDead']
sys.path.insert(0, str(Path(__file__).resolve().parent))
from audit_native_arenas import exports, package as game_package


def package(path):
    s=path.as_posix()
    m=re.search(r'ProjectPBZ/Plugins/(?:Secret/)?([^/]+)/Content/(.+)\.json$',s,re.I)
    return '/'+m[1]+'/'+m[2] if m else game_package(path)


def digest(p):
    with p.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def vec(v, default=None):
    return [float(v[k]) for k in ('X','Y','Z')] if isinstance(v,dict) and all(k in v for k in ('X','Y','Z')) else default


def asset(ref):
    return ref.get('AssetPathName') if isinstance(ref,dict) else None


def pkg_of(ref):
    return ref.rsplit('.',1)[0]


def world_pkg(pkg):
    return pkg.split('/_Generated_/')[0]


def clean(v):
    if isinstance(v,float):
        if not math.isfinite(v): raise ValueError('Nonfinite coordinate')
        return round(v,6) + 0.0
    if isinstance(v,list): return [clean(x) for x in v]
    if isinstance(v,dict): return {k:clean(x) for k,x in v.items()}
    return v


def convex_hull(pts):
    # Exact sort/cross<=0 rule of the native JS. No curve sampling.
    if len(pts)<3: return []
    pts=sorted(pts)
    def cross(o,a,b): return (a[0]-o[0])*(b[1]-o[1])-(a[1]-o[1])*(b[0]-o[0])
    lo=[];hi=[]
    for p in pts:
        while len(lo)>=2 and cross(lo[-2],lo[-1],p)<=0: lo.pop()
        lo.append(p)
    for p in reversed(pts):
        while len(hi)>=2 and cross(hi[-2],hi[-1],p)<=0: hi.pop()
        hi.append(p)
    h=lo[:-1]+hi[:-1]
    return h if len(h)>=3 else []


def signed_area(h):
    return sum(a[0]*b[1]-b[0]*a[1] for a,b in zip(h,h[1:]+h[:1]))*.5


def inset_radial(h,d):
    cx=sum(p[0] for p in h)/len(h);cy=sum(p[1] for p in h)/len(h)
    out=[]
    for x,y in h:
        dx=cx-x;dy=cy-y;dist=math.hypot(dx,dy)
        move=min(d,dist*.9)
        out.append([x,y] if dist<1 else [x+dx/dist*move,y+dy/dist*move])
    return out


def inset_hull(h,d=60):
    if len(h)<3 or d<=0:return h
    s=signed_area(h)
    if s==0:return inset_radial(h,d)
    sg=1 if s>0 else -1;edges=[];lines=[]
    for a,b in zip(h,h[1:]+h[:1]):
        dx=b[0]-a[0];dy=b[1]-a[1];length=math.hypot(dx,dy)
        if length<1e-3:return inset_radial(h,d)
        edges.append([dx,dy]);lines.append([a[0]-dy/length*sg*d,a[1]+dx/length*sg*d])
    out=[]
    for i in range(len(h)):
        j=(i-1)%len(h);a=edges[j];b=edges[i];den=a[0]*b[1]-a[1]*b[0]
        if abs(den)<1e-6:out.append(lines[i][:]);continue
        t=((lines[i][0]-lines[j][0])*b[1]-(lines[i][1]-lines[j][1])*b[0])/den
        out.append([lines[j][0]+a[0]*t,lines[j][1]+a[1]*t])
    return inset_radial(h,d) if signed_area(out)*s<=0 else out


IDENTITY = [[1.,0.,0.,0.],[0.,1.,0.,0.],[0.,0.,1.,0.],[0.,0.,0.,1.]]


def mul(a,b):
    return [[sum(a[i][k]*b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def apply(m,v):
    return [sum(m[i][j]*v[j] for j in range(3))+m[i][3] for i in range(3)]


def compose_trs(parent,local):
    # FTransform concatenation keeps quaternion rotation + componentwise scale;
    # ordinary matrix multiplication would introduce shear for nonuniform scale.
    ps=[math.sqrt(sum(parent[i][j]**2 for i in range(3))) for j in range(3)]
    ls=[math.sqrt(sum(local[i][j]**2 for i in range(3))) for j in range(3)]
    if min(ps+ls)<1e-12:raise ValueError('Zero scale in attachment transform')
    pr=[[parent[i][j]/ps[j] for j in range(3)] for i in range(3)]
    lr=[[local[i][j]/ls[j] for j in range(3)] for i in range(3)]
    loc=apply(parent,[local[i][3] for i in range(3)])
    return [[sum(pr[i][k]*lr[k][j] for k in range(3))*ps[j]*ls[j] for j in range(3)]+[loc[i]] for i in range(3)]+[[0.,0.,0.,1.]]


def matrix(p):
    # Unreal FRotator: left-handed Pitch/Yaw/Roll. Matrix acts on columns.
    r=p.get('RelativeRotation') or {};pitch,yaw,roll=[math.radians(float(r.get(k,0))) for k in ('Pitch','Yaw','Roll')]
    sp,cp=math.sin(pitch),math.cos(pitch);sy,cy=math.sin(yaw),math.cos(yaw);sr,cr=math.sin(roll),math.cos(roll)
    rows=[[cp*cy,sr*sp*cy-cr*sy,-(cr*sp*cy+sr*sy)],
          [cp*sy,sr*sp*sy+cr*cy,cy*sr-cr*sp*sy],
          [sp,-sr*cp,cr*cp]]
    scale=vec(p.get('RelativeScale3D'),[1.,1.,1.]);loc=vec(p.get('RelativeLocation'),[0.,0.,0.])
    return [[rows[i][j]*scale[j] for j in range(3)]+[loc[i]] for i in range(3)]+[[0.,0.,0.,1.]]


def short_export(e):
    """Keep joins/geometry but discard heavyweight render/streaming caches."""
    q={k:v for k,v in e.items() if k in ('Type','Name','Class','Outer','Template','Package','ActorInstanceGuid','ActorLabel','PersistentLevel','StreamingLevels')}
    p=e.get('Properties',{});t=e.get('Type','')
    if 'Component' in t and 'Flow' not in t:
        p={k:v for k,v in p.items() if k in ('RelativeLocation','RelativeRotation','RelativeScale3D','AttachParent','AttachSocketName','bAbsoluteLocation','bAbsoluteRotation','bAbsoluteScale','SplineCurves','bClosedLoop','ComponentToWorld')}
        if 'SplineCurves' in p:p['SplineCurves']={'position':{'Points':p['SplineCurves'].get('position',{}).get('Points',[])}}
    elif 'WorldPartition' in t and t not in ('WorldPartitionRuntimeLevelStreamingCell','WorldPartitionLevelStreamingDynamic','WorldPartitionRuntimeCellDataHashSet'):
        p={k:v for k,v in p.items() if k in ('InstanceTransform','WorldAsset','LevelTransform')}
    elif t=='Level':
        p={k:v for k,v in p.items() if k in ('WorldSettings','WorldPartitionRuntimeCell','bIsPartitioned','WorldDataLayers','LevelTransform','LevelInstanceID')}
    q['Properties']=p
    return q


class Index:
    def __init__(self, paths):
        self.nodes={};self.inputs=[];self.by_soft=defaultdict(list);self.pkg_nodes=defaultdict(list)
        self.errors=[];self.export_counts={};self.transform_cache={};self.default_nodes={}
        seen={}
        for path in paths:
            pkg=package(path)
            if not pkg:continue
            h=digest(path)
            if pkg in seen:
                if seen[pkg]!=h:raise ValueError(f'Conflicting exports of {pkg}')
                continue
            seen[pkg]=h
            self.inputs.append({'package':pkg,'file':path.resolve().as_posix(),'sha256':h,'bytes':path.stat().st_size})
            count=0
            for i,e in enumerate(exports(path)):
                count+=1;t=e.get('Type','');p=e.get('Properties',{})
                keep=(t in ('World','Level','WorldDataLayers','MobGroup','DataLayerInstanceWithAsset') or
                      'RootComponent' in p or 'Component' in t or
                      any(s in t for s in ('Spawner','FogDoor','FlowActor','Trigger','SequenceActor','BossCombat','MobGroupScript','GroupScript','LevelInstance','WorldPartition')))
                if keep or not ('/Maps/' in pkg):
                    key=f'{pkg}.{i}';self.nodes[key]=short_export(e);self.pkg_nodes[pkg].append(key)
                    if not ('/Maps/' in pkg):self.default_nodes[key]=e
            self.export_counts[pkg]=count
            print(f'indexed {pkg}: {count} exports',file=sys.stderr,flush=True)
        for key,e in self.nodes.items():
            path=self.subpath(key)
            if path:self.by_soft[(world_pkg(pkg_of(key)),path)].append(key)
        self.streams={}
        for key,e in self.nodes.items():
            if e['Type']=='WorldPartitionLevelStreamingDynamic':
                target=e['Properties'].get('PackageNameToLoad')
                if target:self.streams[target]=key
        self.group_members=defaultdict(list)
        for k,e in self.nodes.items():
            if e['Type']=='MobGroup':
                for ref in e['Properties'].get('Members',[]):
                    s=self.resolve(ref,k)
                    if s:self.group_members[s].append(k)

    @lru_cache(None)
    def subpath(self,key):
        e=self.nodes.get(key)
        if not e:return None
        outer=e.get('Outer',{});name=outer.get('ObjectName','')
        if "'" in name:
            tail=name.split("'",1)[1].rstrip("'")
            if ':' in tail:return tail.split(':',1)[1]+'.'+e['Name']
        if e['Type']=='Level':return e['Name']
        return None

    def resolve(self,ref,context=None):
        if not isinstance(ref,dict):return None
        direct=ref.get('ObjectPath')
        if direct in self.nodes:return direct
        ap=asset(ref) or '';sub=ref.get('SubPathString','')
        if not sub or not ap:return None
        pkg=ap.split('.',1)[0];world=world_pkg(pkg)
        c=self.by_soft.get((world,sub),[])
        if len(c)==1:return c[0]
        exact=[k for k in c if pkg_of(k)==pkg]
        return exact[0] if len(exact)==1 else None

    def ref(self,key):
        if not key:return None
        e=self.nodes.get(key)
        if not e:return {'source_ref':key,'resolved':False}
        pkg=pkg_of(key);wp=world_pkg(pkg);name=wp.rsplit('/',1)[-1]
        sub=self.subpath(key)
        return {'object_path':f'{pkg}.{name}:{sub}' if sub else key,'source_ref':key,
                'class':e.get('Class',e['Type']),'type':e['Type'],'name':e['Name'],'source_package':pkg,'resolved':True}

    def level_context(self,key):
        pkg=pkg_of(key);wp=world_pkg(pkg);name=wp.rsplit('/',1)[-1]
        levels=[k for k in self.pkg_nodes[pkg] if self.nodes[k]['Type']=='Level']
        level=levels[0] if len(levels)==1 else None
        st=self.streams.get(pkg);cell=self.resolve(self.nodes[st]['Properties'].get('StreamingCell'),st) if st else None
        if '/_Generated_/' in pkg:
            reason='WP cell LevelTransform omitted; native identity default and absence of a parent LevelInstance are not independently proven by this JSON export'
            verified=False
        else:
            verified=bool(level and name.endswith('_WP') and self.nodes[level]['Properties'].get('bIsPartitioned'))
            reason=None if verified else 'Non-partition root/sublevel or owning world placement not resolved'
        return {'world':name,'world_package':wp,'level_ref':level,'source_package':pkg,
                'streaming_ref':st,'runtime_cell_ref':cell,'world_coordinates_verified_static':verified,
                'pending_reason':reason}

    def props(self,key):
        return self.nodes[key]['Properties']

    def effective(self,key,trail=()):
        if key in trail:return {},[]
        e=self.nodes[key];template=self.resolve(e.get('Template'),key)
        base,refs=self.effective(template,trail+(key,)) if template else ({},[])
        return {**base,**e['Properties']},refs+[key]

    def component_matrix(self,key,trail=()):
        if key in trail:return None,[],['attachment_cycle']
        e=self.nodes.get(key)
        if not e:return None,[],['missing_component']
        p=e['Properties'];chain=[{'source_ref':key,'relative_location':p.get('RelativeLocation'),
            'relative_rotation':p.get('RelativeRotation'),'relative_scale':p.get('RelativeScale3D'),
            'attach_parent':p.get('AttachParent'),'omitted_axes_use_template_or_native_defaults':True}]
        # BP root templates here have identity local rotation/scale; absent local
        # position on an unattached actor root is not promoted to world (0,0,0).
        parent=self.resolve(p.get('AttachParent'),key)
        if p.get('AttachSocketName') not in (None,'None',''):
            return None,chain,['socket_transform_not_exported']
        if any(p.get(f'bAbsolute{x}') for x in ('Location','Rotation','Scale')):
            return None,chain,['absolute_component_axes_require_native_FTransform_composition']
        if min(vec(p.get('RelativeScale3D'),[1,1,1]))<=0:
            return None,chain,['negative_or_zero_scale_native_matrix_path_not_verified']
        m=matrix(p)
        if p.get('AttachParent'):
            if not parent:return None,chain,['unresolved_attach_parent']
            pm,pc,err=self.component_matrix(parent,trail+(key,))
            return (compose_trs(pm,m) if pm else None),chain+pc,err
        if vec(p.get('RelativeLocation')) is None:
            return None,chain,['unattached_root_location_not_serialized']
        return m,chain,[]

    @lru_cache(None)
    def pose(self,key):
        if not key:return {'position_world':None,'position_level':None,'world_coordinates_verified_static':False,'notes':['unresolved_object']}
        e=self.nodes[key];p=e['Properties']
        root=self.resolve(p.get('RootComponent'),key)
        if not root and 'Component' in e['Type']:root=key
        if not root:
            return {'position_world':None,'position_level':None,'world_coordinates_verified_static':False,'notes':['root_component_not_resolved']}
        m,chain,err=self.component_matrix(root);ctx=self.level_context(key)
        verified=m is not None and not err and ctx['world_coordinates_verified_static']
        loc=apply(m,[0,0,0]) if m else None
        direction=[m[i][0] for i in range(3)] if m else None
        return {'position_world':loc if verified else None,'position_level':loc,
                'forward_world':direction if verified else None,'forward_level':direction,
                'yaw_degrees_level':math.degrees(math.atan2(direction[1],direction[0])) if direction else None,
                'world_coordinates_verified_static':verified,'runtime_verified':False,
                'transform_chain':chain,'level_context':ctx,'notes':err+([ctx['pending_reason']] if ctx['pending_reason'] else [])}

    def group_for(self,spawner):
        p=self.props(spawner);g=self.resolve(p.get('MobGroup'),spawner)
        methods=['spawner.MobGroup'] if g else []
        domain=self.resolve(p.get('SpawnInfo',{}).get('MobDomain'),spawner)
        group_name=p.get('SpawnInfo',{}).get('MobGroupName','DefaultGroup')
        if not g and domain:
            for kv in self.props(domain).get('MobDomainConfig',{}).get('MobGroups',[]):
                if kv.get('Key')==group_name:
                    g=self.resolve(kv.get('Value'),domain)
                    if g:methods.append('SpawnInfo.MobDomain.MobDomainConfig[MobGroupName/default DefaultGroup]')
        reverse=self.group_members.get(spawner,[])
        if not g and len(reverse)==1:g=reverse[0];methods.append('unique reverse MobGroup.Members')
        conflicts=[x for x in reverse if g and x!=g]
        return g,domain,group_name,methods,conflicts

    def group(self,key,spawner=None):
        if not key:return None
        p=self.props(key);members=[]
        for ref in p.get('Members',[]):
            s=self.resolve(ref,key);m={'reference':ref,'spawner':self.ref(s),'spawn_class':asset(self.props(s).get('Class')) if s else None}
            if s:m['pose']=self.pose(s)
            members.append(m)
        scripts=[]
        for ref in p.get('Scripts',[]):
            s=self.resolve(ref,key);t=self.nodes[s]['Type'] if s else None
            role='phase_or_boss_combat' if t and 'BossCombat' in t else 'combat' if t and any(x in t for x in ('Combat','FogDoor','ClearGroup')) else 'unclassified'
            scripts.append({'reference':ref,'script':self.ref(s),'class_name':t,'role_by_name':role,
                'behavior_verified':False,'properties':self.props(s) if s else None})
        ids=set(m['spawn_class'] for m in members if m['spawn_class'] in self.bosses)
        identities=sorted({self.bosses[c]['identity'] for c in ids})
        return {**self.ref(key),'members':members,'scripts':scripts,
                'member_list_serialized':'Members' in p,'all_members_resolved':all(m['spawner'] for m in members),
                'target_in_members':any(m['spawner'] and m['spawner']['source_ref']==spawner for m in members) if spawner else None,
                'boss_classes':sorted(ids),'boss_identities':identities,
                'multiple_enemy_members':len(members)>1,
                'member_classes_outside_boss_catalog':sorted({m['spawn_class'] for m in members if m['spawn_class'] and m['spawn_class'] not in self.bosses}),
                'multi_boss_identity':len(identities)>1,
                'multiple_boss_classes':len(ids)>1,
                'simultaneous_or_phase_order':'待核; group membership alone does not prove simultaneous combat'}

    def door(self,ref,script):
        key=self.resolve(ref,script)
        if not key:return {'reference':ref,'resolved':False,'position_world':None,'spline_points_world':None,'notes':['door reference unresolved']}
        p=self.props(key);spline=self.resolve(p.get('Spline'),key);sp=self.props(spline) if spline else {}
        pts=[vec(x.get('OutVal')) for x in sp.get('SplineCurves',{}).get('position',{}).get('Points',[])]
        pts=[x for x in pts if x is not None]
        m,chain,err=self.component_matrix(spline) if spline else (None,[],['spline_not_resolved'])
        levelpts=[apply(m,x) for x in pts] if m else None;pose=self.pose(key)
        verified=bool(m and not err and self.level_context(key)['world_coordinates_verified_static'])
        # Never silently treat actor cached "Spline Points Locations" as world.
        return {**self.ref(key),'link_script_ref':script,'pose':pose,'spline_ref':spline,'spline_component_transform_chain':chain,
                'spline_points_local':pts,'spline_points_level':levelpts,
                'spline_points_world':levelpts if verified else None,
                'world_coordinates_verified_static':verified,'bIsClosedSpline':p.get('bIsClosedSpline'),
                'spline_bClosedLoop':sp.get('bClosedLoop'),'cached_Spline_Points_Locations':p.get('Spline Points Locations'),
                'notes':err+([] if 'bIsClosedSpline' in p else ['bIsClosedSpline not serialized; do not infer from a name'])}

    def doors(self,g):
        out={ev:[] for ev in EVENTS};out['link_scripts']=[]
        for ref in self.props(g).get('Scripts',[]) if g else []:
            s=self.resolve(ref,g)
            if not s or 'LinkFogDoor' not in self.nodes[s]['Type']:continue
            p,templates=self.effective(s);out['link_scripts'].append({'source_ref':s,'bRequirePlayerInside':p.get('bRequirePlayerInside'),
                'property_template_chain':templates,'serialized_fields':sorted(self.props(s)),
                'fields_not_serialized':[e for e in EVENTS if e not in p]})
            for ev in EVENTS:
                for d in p.get(ev,[]):out[ev].append(self.door(d,s))
        out['activation_link_status']='explicit' if out['FogDoorsToActivate'] else 'no_activate_reference_in_resolved_instance_scripts'
        return out

    def hull(self,doors,spawner):
        activate=doors['FogDoorsToActivate'];note=None
        if not activate:return None,'No serialized FogDoorsToActivate reference; absence is not proof that the encounter has no doors'
        scripts=sorted({d.get('link_script_ref') for d in activate if d.get('link_script_ref')})
        if len(scripts)>1:
            regions=[]
            for script in scripts:
                h,reason=self.hull({'FogDoorsToActivate':[d for d in activate if d.get('link_script_ref')==script]},spawner)
                regions.append({'link_script_ref':script,'hull':h,'null_reason':reason})
            valid=[r['hull'] for r in regions if r['hull']]
            if not valid:return None,'All individual LinkFogDoor script hulls are degenerate or unresolved'
            result=dict(valid[0]) if len(valid)==1 else {'xy_polygon_world':None,'xy_polygon_level':None,
                'area_uu2':None,'area_m2':None,'world_coordinates_verified_static':False}
            result['per_link_script_hulls']=regions
            result['selection_rule']='One valid script hull retained' if len(valid)==1 else 'Multiple native script regions; no union guessed'
            return result,None
        if not all(d.get('resolved') and d.get('spline_points_level') is not None for d in activate):
            return None,'An activation door/spline transform is unresolved; no partial hull is promoted'
        frames={d['source_package'] for d in activate}
        if len(frames)>1 and not all(d['world_coordinates_verified_static'] for d in activate):
            return None,'Activation doors span unresolved coordinate frames; no mixed-level hull is promoted'
        pts=[p for d in activate if len(d['spline_points_level'])>=2 for p in d['spline_points_level']]
        raw=convex_hull([[p[0],p[1]] for p in pts]);a=abs(signed_area(raw));valid=len(raw)>=3 and a>=10000
        if not valid:return None,'Native JS rejects hull with <3 vertices or area <10000 uu2'
        h=inset_hull(raw,60);verified=all(d['world_coordinates_verified_static'] for d in activate)
        z=[p[2] for p in pts];pose=self.pose(spawner);pos=pose['position_level']
        return {'xy_polygon_world':h if verified else None,'xy_polygon_level':h,
                'raw_xy_polygon_level':raw,'raw_xy_polygon_world':raw if verified else None,
                'area_uu2':abs(signed_area(h)),'area_m2':abs(signed_area(h))/10000,
                'raw_area_uu2':a,'door_control_point_z_range_level':[min(z),max(z)],
                'door_control_point_z_range_world':[min(z),max(z)] if verified else None,
                'ground_surface_height_verified':False,'z_range_is_not_a_ground_trace':True,
                'spawner_minus_door_z_range_uu':[pos[2]-max(z),pos[2]-min(z)] if pos and (frames=={pkg_of(spawner)} or (verified and pose['world_coordinates_verified_static'])) else None,
                'world_coordinates_verified_static':verified,'inset_uu':60,'minimum_raw_area_uu2':10000,
                'algorithm_source':JS.relative_to(ROOT).as_posix(),'runtime_verified':False},None

    def risk_candidates(self):
        rows=[];flowowners=defaultdict(list)
        for k,e in self.nodes.items():
            if 'FlowComponent' in e['Type']:
                o=self.resolve(e.get('Outer'),k)
                if o:flowowners[o].append(k)
        for k,e in self.nodes.items():
            t=e['Type'];p=e['Properties'];cats=[];spawn=asset(p.get('Class'))
            if 'Spawner' in t and (spawn in self.bosses or (spawn and any(s in spawn for s in ('YuCong_P3','ChiLian')))):cats.append('boss_spawner')
            if k in flowowners:cats.append('flow_actor')
            if 'Trigger' in t and ('RootComponent' in p or 'Volume' in t or t=='TriggerBox'):cats.append('trigger')
            if 'SequenceActor' in t:cats.append('level_sequence_actor')
            if t=='MobGroup':cats.append('mob_group')
            if not cats:continue
            owner=k
            if t=='MobGroup':owner=self.resolve(p.get('MobDomain'),k) or self.resolve(p.get('MobDomainActor'),k) or self.resolve(e.get('Outer'),k)
            pose=self.pose(owner) if owner else self.pose(None)
            rows.append((k,cats,pose,flowowners.get(k,[]),owner,spawn))
        return rows

    def risk_descriptor(self,k,cats,pose,flow,owner,spawn):
        out={**self.ref(k),'categories':cats,'position_world':pose['position_world'],'position_level':pose['position_level'],
             'world_coordinates_verified_static':pose['world_coordinates_verified_static'],
             'root_component_ref':pose.get('transform_chain',[{}])[0].get('source_ref') if pose.get('transform_chain') else None,
             'coordinate_notes':pose.get('notes',[]),'flow_components':flow}
        if spawn:out['spawn_class']=spawn
        if cats==['mob_group']:out['position_source_domain']=owner
        return out

    def risks(self,spawner,g,candidates):
        wp=world_pkg(pkg_of(spawner));pos=self.pose(spawner);origin=pos['position_level']
        out={'radius_m':150,'distance_metric':'3D Euclidean actor/root origin; not trigger extent or nav distance',
             'evidence':'静态','world_coordinates_verified_static':pos['world_coordinates_verified_static'],
             'nearby':[],'same_level_radius_candidates':[],'distance_pending':[],
             'coverage':'待核: only exported packages; unexported cells / dynamic actors may contain additional risks'}
        for k,cats,pose,flow,owner,spawn in candidates:
            if world_pkg(pkg_of(k))!=wp or k==spawner or k==g:continue
            target=pose['position_level'];compatible=(pkg_of(k)==pkg_of(spawner) or (pose['world_coordinates_verified_static'] and pos['world_coordinates_verified_static']))
            row=self.risk_descriptor(k,cats,pose,flow,owner,spawn)
            if 'boss_spawner' in cats:
                other=self.group_for(k)[0];row.update({'spawn_class':spawn,'mob_group_ref':other,
                    'same_mob_group':g==other if g and other else None})
            if cats==['mob_group']:row['position_source_domain']=owner
            if not origin or not target or not compatible:
                # Preserve candidates which cannot safely be radius-filtered.
                out['distance_pending'].append(k);continue
            dist=math.dist(origin,target)/100
            if dist<=150:
                row['distance_m']=dist
                row['distance_verified_world']=pose['world_coordinates_verified_static'] and pos['world_coordinates_verified_static']
                row['distance_space']='world_static' if row['distance_verified_world'] else 'same_cooked_level'
                out['nearby' if row['distance_verified_world'] else 'same_level_radius_candidates'].append(row)
        out['nearby'].sort(key=lambda x:(x['distance_m'],x['source_ref']))
        return out


def render(idx):
    coverage=json.loads(COVERAGE.read_text(encoding='utf-8'))['items'];assert len(coverage)==77
    links=json.loads(LINKS.read_text(encoding='utf-8'))['items'];byid={x['id']:x for x in links}
    idx.bosses={b['path']:b for b in json.loads((ROOT/'mods/tower/data/bosses.json').read_text(encoding='utf-8'))['items']}
    risks=idx.risk_candidates();items=[];groups={}
    for b in coverage:
        old=byid[b['placement']];s=old['source_ref']
        if s not in idx.nodes:raise ValueError(f'Missing selected 32ffb7d source {s}')
        actual=asset(idx.props(s).get('Class'))
        if actual!=b['path']:raise ValueError(f'Class mismatch for {s}')
        g,domain,gn,method,conflict=idx.group_for(s);group=idx.group(g,s)
        if group:groups[g]=group
        doors=idx.doors(g);hull,hullreason=idx.hull(doors,s);ctx=idx.level_context(s)
        alternatives_groups=[idx.group(k,s) for k in conflict]
        if conflict:
            hull=None;hullreason='Spawner.MobGroup disagrees with reverse MobGroup.Members; authoritative encounter group 待核'
            doors['activation_link_status']='待核_group_conflict; listed doors belong to declared group only'
        notes=[]
        if b.get('scene'):notes.append('Catalog flags scene-dependent Boss; independent tower generation dependencies 待核')
        if re.search(r'_P[123]|FirstTime|HunDun|Bing|WuLi',b['path']):notes.append('Phase/body/encounter variant name; native phase handoff dependencies 待核, never infer phase ordering from a suffix')
        if conflict:notes.append('Conflicting reverse member groups: '+', '.join(conflict))
        if not g:notes.append('MobGroup unresolved after direct, domain-name/default and reverse-membership joins')
        if hullreason:notes.append(hullreason)
        if not ctx['world_coordinates_verified_static']:notes.append(ctx['pending_reason'])
        if group and not group['target_in_members']:notes.append('Selected spawner is not in serialized Members; do not assume no membership at runtime')
        alternatives=[{'source_ref':x['source_ref'],'world':x['level'],'spawner_id':x.get('spawner'),
                      'group_ref_previous_audit':x.get('group_ref'),'challenge_candidate_previous_audit':x['challenge_candidate']}
                      for x in links if x['class_path']==b['path'] and x['source_ref']!=s]
        item={'boss_class':b['path'],'identity':b['identity'],
              'evidence':'静态','runtime_verified':False,'world':ctx['world'],'data_layer':idx.layers(s),
              'source_package':ctx['source_package'],'selection':{'source':'boss_live_coverage_32ffb7d.json exact placement','placement':b['placement'],'runtime_suitability':'待核'},
              'spawner':{**idx.ref(s),'spawner_id':idx.props(s).get('SpawnerID'),'pose':idx.pose(s),
                         'should_spawn_serialized':idx.props(s).get('ShouldSpawn'),'spawn_info':idx.props(s).get('SpawnInfo')},
              'mob_group':group,'mob_group_join':{'status':'待核_conflict' if conflict else '静态' if g else '待核_unresolved',
                  'group_name':gn,'domain_ref':domain,'methods':method,'conflicting_reverse_groups':conflict},
              'conflicting_member_groups':alternatives_groups,
              'fog_doors':doors,'arena_hull':hull,'arena_hull_null_reason':hullreason,
              'story_risk':idx.risks(s,g,risks),'alternate_placements':alternatives,'notes':notes}
        items.append(item)
    worlds=defaultdict(set)
    for k,e in idx.nodes.items():
        c=asset(e['Properties'].get('Class'))
        if 'Spawner' in e['Type'] and c in idx.bosses and 'PSO' not in k:worlds[world_pkg(pkg_of(k))].add(idx.bosses[c]['identity'])
    summary={'boss_classes':77,'identities':len({x['identity'] for x in items}),
             'classes_with_activate_doors':sum(bool(x['fog_doors']['FogDoorsToActivate']) for x in items),
             'classes_without_resolved_activate_link':sum(not x['fog_doors']['FogDoorsToActivate'] for x in items),
             'classes_proven_no_doors':0,'classes_with_computable_level_hull':sum(x['arena_hull'] is not None for x in items),
             'classes_with_group_membership_conflict':sum(bool(x['conflicting_member_groups']) for x in items),
             'classes_with_world_coordinate_chain_verified_static':sum(x['spawner']['pose']['world_coordinates_verified_static'] for x in items),
             'selected_distinct_groups_with_multiple_boss_identities':sum(g['multi_boss_identity'] for g in groups.values()),
             'selected_distinct_groups_with_multiple_boss_classes':sum(g['multiple_boss_classes'] for g in groups.values()),
             'worlds_with_multiple_boss_identities':sum(len(v)>1 for v in worlds.values()),
             'selected_worlds_with_multiple_boss_identities':sum(len(worlds[w])>1 for w in {world_pkg(x['source_package']) for x in items}),
             'selected_placements_with_other_identity_within150m_world_static':sum(any('boss_spawner' in r['categories'] and idx.bosses.get(r.get('spawn_class'),{}).get('identity')!=x['identity'] for r in x['story_risk']['nearby']) for x in items)}
    data={'schema':1,'evidence':'静态','scope':'77 exact classes and selected placements from boss_live_coverage_32ffb7d; no engine/Core/UI/shared edits, no process access',
          'policy':'Venue+fog only; tower owns Boss spawning. Native encounters must not be started. Static catalog is not runtime acceptance.',
          'coordinate_policy':{'units':'Unreal uu=cm, metres=uu/100','unverified_world_values':None,
              'level_coordinates':'Resolved component attachment chain in serialized cooked level; not automatically world coordinates',
              'generated_cell_policy':'Missing LevelTransform / LevelInstance identity proof is marked 待核; do not reuse position_level as world',
              'height_policy':'Door control point Z extrema, not navmesh/ground/walkable height proof'},
          'source_files':idx.inputs,'dependencies':[{'file':p.relative_to(ROOT).as_posix(),'sha256':digest(p)} for p in [COVERAGE,LINKS,JS,ROOT/'mods/tower/data/bosses.json',Path(__file__)]],
          'summary':summary,'input_coverage':{'packages':len(idx.inputs),'exports_scanned':sum(idx.export_counts.values()),
              'complete_world_export':False,'unexported_cells':'Not assumed empty. Nearby query completeness remains 待核.'},
          'multi_boss_groups':[g for g in groups.values() if g['multiple_boss_classes']],
          'story_risk_objects':{k:idx.risk_descriptor(k,cats,pose,flow,owner,spawn) for k,cats,pose,flow,owner,spawn in risks},
          'world_coexistence':[{'world_package':w,'identities':sorted(v)} for w,v in sorted(worlds.items()) if len(v)>1],
          'items':items}
    return clean(data)


def layers(self,key):
    ctx=self.level_context(key);cell=ctx['runtime_cell_ref'];p=self.props(key)
    raw=self.props(cell).get('DataLayers',{}) if cell else {};names=raw.get('DataLayers',[]) if isinstance(raw,dict) else raw
    rows=[]
    for name in names:
        c=[k for k in self.pkg_nodes.get(ctx['world_package'],[]) if self.nodes[k]['Type']=='DataLayerInstanceWithAsset' and self.nodes[k]['Name']==name]
        rows.append({'name':name,'source_ref':c[0] if len(c)==1 else None,
                     'properties':self.props(c[0]) if len(c)==1 else None})
    return {'source_package':ctx['source_package'],'runtime_cell_ref':cell,'streaming_ref':ctx['streaming_ref'],
            'actor_data_layer_assets':p.get('DataLayerAssets'),'cell_data_layers':rows,
            'status':'静态' if rows or p.get('DataLayerAssets') else '待核; no actor/cell layer assignment resolved; source package retained'}
Index.layers=layers


def report(data):
    s=data['summary'];lines=['# 影塔场地静态提取', '',
       '证据等级：静态；运行时均未验证。本步不启动游戏、不连接管道、不修改 Core、引擎、UI 或 shared。', '',
       '按用户最新方案：仅借用场地与雾门，Boss 由塔生成，原生遭遇不启动。商店／道具链验收暂缓。', '',
       '## 统计口径', '',
       '每条对应 32ffb7d 覆盖表中的一个精确类及其当时选中的出生点。备选出生点只保留引用，不自动替换。', '',
       '| 项目 | 数量 |','|---|---:|']
    labels={'boss_classes':'精确 Boss 类','identities':'Boss 身份',
       'classes_with_activate_doors':'找到明确激活雾门关联的类',
       'classes_without_resolved_activate_link':'未找到激活门关联（待核）的类',
       'classes_proven_no_doors':'已证明原生没有门的类',
       'classes_with_computable_level_hull':'可计算单元内边界的类',
       'classes_with_group_membership_conflict':'组指针与成员表冲突的类',
       'classes_with_world_coordinate_chain_verified_static':'出生点世界变换链静态闭合的类',
       'selected_distinct_groups_with_multiple_boss_identities':'选中组内多 Boss 身份组（按目录去重）',
       'selected_distinct_groups_with_multiple_boss_classes':'选中组内多 Boss 精确类组（含同身份阶段）',
       'worlds_with_multiple_boss_identities':'全部已查世界中的多 Boss 共图世界（含备选图）',
       'selected_worlds_with_multiple_boss_identities':'77 条选中场地中的多 Boss 共图世界',
       'selected_placements_with_other_identity_within150m_world_static':'静态世界坐标证明 150m 内有其他 Boss 身份的选中点'}
    for k,value in s.items():lines.append(f'| {labels[k]} | {value} |')
    lines += ['', '**“无关联”不等于“确认没有门”。** 单个生成单元、未导出的邻近单元、未解析的默认脚本或动态 Flow 仍可能补出门。', '',
       f"**坐标分层。** 主 WP 持久关卡的未附着根可闭合到静态世界坐标；生成单元的组件链可得 level 坐标，但缺少独立的 LevelTransform/LevelInstance 证明时世界坐标留空。不将旧相对坐标重新命名为世界坐标。{s['classes_with_world_coordinate_chain_verified_static']} 条出生点的静态世界链已闭合，其余 {77-s['classes_with_world_coordinate_chain_verified_static']} 条待核；这不包含实机坐标验证。", '',
       '**凸包。** 按游戏 JS 的 monotone chain（cross <= 0 去共线）、面积门槛 10000 uu²，再按边法线内缩 60 uu；保留原始多边形。Z 范围取激活门的控制点，不代表已经取得地面高度。', '',
       '**成员。** 同组包含多个 Boss 类可能是换体／阶段；只有多个目录 identity 才另计为多 Boss 身份组，仍不证明同时开打。跨组串场不能据此改成双 Boss。统计使用目录中的 Boss 身份，组内其他成员（例如金面、铁面）仍完整列出，不擅自提升为池内 Boss。', '',
       '## 关键结论', '',
       '- **混沌与白赤练不同组。** YuCong_P3_1 / 2 / 3 是 spawner ID，分别刷冰、混沌、武力；三者同属 `.1509`，成员没有 ChiLian。ChiLian_Seq034 与 ChiLianP2_Seq035 分别属于主图 `.1375`、`.1376`，与混沌不是同组。它们的雾门位置十分接近，但坐标近不构成成员关系；不能把串场默认解释为混沌的原生增援。',
       '- **玉鼎与雷当不同组。** Ximeng 的玉鼎 `.1522`，雷当本体和 P2 同属 `.1517`；两组使用不同雾门。此证据不支持“玉鼎＋雷当同组双 Boss”，也不证明完整剧情的车轮战顺序。',
       '- **彭猛确实有关联门。** PengMeng_Seq014 的 `.1690` 组通过 LinkFogDoor 明确引用同单元的 `.775`、`.776` 两扇门；之前实机没有关门，不能归因于原生没门。',
       '- **玉面郎君属于原生多人组。** JadeIntro_Jade 的 `.1297` 组还包含 GoldMaskMan、IronMaskMan。门 `.593` 为闭合样条。该单元中出生点与门控制点 Z 范围相差约 -19.5～45.0 uu，静态上没有明显层高错位；世界变换、地面和玩家落点仍待核，不能因此认定落点已可用。',
       '- **骨婉约按脚本分别算。** 一个组挂两份 LinkFogDoor，不能合并为单一原生凸包。小雪、烟千岁、梦华也已保留全部成员、组脚本、激活／解除门和邻近风险引用。',
       '- **未找到激活门的 5 类**：Danghui、EMei_Sword、MobMan_Elite、ZuoShang、MBoss。只表示本次选中摆放没有解析到关联，不是“全游戏确认无门”。', '',
       '- **三条组指针／成员表需要进一步核对**：PowerMan_P2 的 spawner.MobGroup 指向 Duanhun `.6436`，反向 Members 在 `.6437`；ZuoShang_P2 分别为 `.6438`／`.6439`；MuXiaoKui_P2 也出现在指针组以外的另一份 Members 中。相关完整组均保留，主 arena_hull 留 null，不猜应采用哪组。这可能是原生阶段换组或多组复用，不直接当成资源错误。', '',
       '## 重点对象', '', '| identity / 精确类 | 世界 | MobGroup source_ref | 激活门 | 凸包 | 备注 |','|---|---|---|---:|---|---|']
    for x in data['items']:
        if x['identity'] not in {'YuCong','ChiLian','FemaleVessel_YuDing','LeiDang','XiaoXue','YanQianSui','GuWanYue','MengHua','PengMeng_Axe','JadeMaskMan'}:continue
        g=x['mob_group'];notes='；'.join(x['notes'])
        lines.append(f"| {x['identity']} / {x['boss_class'].rsplit('.',1)[-1]} | {x['world']} | {g['source_ref'] if g else '待核'} | {len(x['fog_doors']['FogDoorsToActivate'])} | {'level 已算 / world 待核' if x['arena_hull'] and not x['arena_hull']['world_coordinates_verified_static'] else '静态' if x['arena_hull'] else 'null'} | {notes} |")
    lines += ['', '## 重建与检查', '', '```powershell',
       'python -I mods/tower/tools/build_venues.py',
       'python -I mods/tower/tools/build_venues.py --check',
       '```', '', '`--dump <root>` 可重复，改用迁移后的完整 CUE4Parse 导出目录；按包路径去重，相同包不同 SHA256 直接失败。--check 重建后比较 JSON 与本摘要，失败不写文件。', '',
       '输入包、文件 SHA256、export index 引用、脚本来源均在 venues.json。story_risk.nearby 仅含静态世界坐标可比较且在 150m 内的对象；same_level_radius_candidates 是同单元坐标中的候选，世界距离仍待核；distance_pending 引用顶层 story_risk_objects，不把它们算成“已在 150m 内”。', '',
       '## 补充导出的复现', '',
       '现有 30 个 bossmap 包之外，本轮补出两个主 WP 包和 29 个组脚本蓝图，均来自本机游戏归档。读取现有本机密钥及 usmap，不输出密钥，不启动游戏。原始导出留在 build 工作目录；它们是重建输入，不是发布内容。', '',
       '```powershell',
       "& E:/PBZ-claude/tools/extract/bin/Release/net10.0/PBZExtract.exe grep E:/Phantom_Blade_Zero/build/tower-venues/maps '^ProjectPBZ/Content/Maps/(B01/PBZ_WP_ZhuLinGuWu|DengDingShanDao/PBZ_DengDingShanDao_WP)\\.umap$' '.'",
       "& E:/PBZ-claude/tools/extract/bin/Release/net10.0/PBZExtract.exe export E:/Phantom_Blade_Zero/build/tower-venues/defaults '/(MobGroupScript_[^/]+|BossCombat_[^/]+|GroupScript_[^/]+)\\.uasset$'", '```', '',
       '本机旧的主工作区 PBZExtract.exe 仍不支持 grep；上面的路径固定到 Claude 已有的 grep 版本，未重建或改动提取器。', '',
       '后续必须补核邻近未导出单元、实例变换、有效地面／玩家落点、门能否单独开关，以及场地依赖和阶段换体。当前不得直接将全表作为运行时白名单。', '',
       '## 离线校验', '',
       '`python -I mods/tower/tools/test_venues.py`：包含对游戏原始 JS 的独立 Node 执行对照、负坐标和旋转／非均匀缩放、附着父级、缺失单元变换不冒充世界坐标、DefaultGroup／反向成员和重复组件名、PlayerDead 字段及退化凸包检查。此类检查不提升为实机证据。', '']
    return '\n'.join(lines)


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--dump',type=Path,action='append');ap.add_argument('--defaults',type=Path,action='append');ap.add_argument('--check',action='store_true');args=ap.parse_args()
    roots=args.dump or [ROOT.parent/'PBZ-claude/build/bossmap',ROOT/'build/tower-venues/maps',ROOT/'notes/dump/tower-growth']
    paths=sorted({p.resolve() for root in roots for p in root.rglob('*.json') if package(p) and '/Maps/' in p.as_posix()})
    defaults=args.defaults or [ROOT/'build/tower-venues/defaults']
    paths+=sorted({p.resolve() for root in defaults for p in root.rglob('*.json') if package(p)})
    paths += [ROOT/p for p in [
        'notes/dump/enemies/ProjectPBZ/Content/PBZContent/Blueprint/Item/BP_FogDoor.json',
        'notes/dump/enemies/ProjectPBZ/Plugins/Secret/SGameAI/Content/Tools/Spawner/BP_AISpawner.json',
        'notes/dump/enemies/ProjectPBZ/Plugins/Secret/SGameAI/Content/Tools/MobDomain/BP_MobDomain.json']]
    if not paths:raise SystemExit('No complete map exports found; supply --dump')
    idx=Index(paths);data=render(idx)
    payload=(json.dumps(data,ensure_ascii=False,indent=2,allow_nan=False)+'\n').encode('utf-8');md=report(data).encode('utf-8')
    if args.check:
        for p,want in [(DATA,payload),(REPORT,md)]:
            if not p.exists() or p.read_bytes().replace(b'\r\n',b'\n')!=want:raise SystemExit(f'Stale: {p}')
        print('CHECK PASS: exact 77 classes, sources, transforms, joins, hulls and both regenerated deliverables')
    else:
        DATA.write_bytes(payload);REPORT.write_bytes(md);print(json.dumps(data['summary'],ensure_ascii=False))


if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8');sys.stderr.reconfigure(encoding='utf-8');main()
