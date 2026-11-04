#!/usr/bin/env python3
"""Install/restore the QCA6490 BlueTool service, the type-0 bluetoothd and RenoirBTType0.dylib in an offline 20H392 root."""
import argparse,hashlib,os,plistlib,shutil
from pathlib import Path
FILES={'BlueTool':('usr/sbin/BlueTool','5eec00a4a2a710688bc5ee453c1f3c875966374258b3d455f4347a7458cbff90'),
       'bluetoothd':('usr/sbin/bluetoothd','8b7c0a0bea51544db82269d7f2775ab7996b8740927283428c4753e032df3a8f')}
ADDED={'RenoirBTType0.dylib':'usr/lib/RenoirBTType0.dylib'}
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def replace(source,target):
    stage=target.with_name(target.name+'.renoir-new')
    shutil.copyfile(source,stage);os.chmod(stage,0o755)
    os.replace(stage,target)
def install(root,source):
    backup=root/'.renoir-bluetool-backup';backup.mkdir(exist_ok=True)
    for name,(rel,original) in FILES.items():
        built=source/name
        if sha(built)!=(source/(name+'.sha256')).read_text().strip():raise ValueError(f'{name}: build hash differs; rebuild')
        target=root/rel;saved=backup/name
        if not saved.exists():
            if sha(target)!=original:raise ValueError(f'unsupported {name}: expected audited 20H392 original')
            shutil.copy2(target,saved)
        elif sha(saved)!=original:raise ValueError(f'{name}: original backup hash differs')
    for name,rel in ADDED.items():
        if sha(source/name)!=(source/(name+'.sha256')).read_text().strip():raise ValueError(f'{name}: build hash differs; rebuild')
        target=root/rel
        if target.exists() and not (backup/(name+'.installed')).exists():raise ValueError(f'unmanaged {rel} already exists')
    for name,rel in ADDED.items():
        replace(source/name,root/rel);(backup/(name+'.installed')).write_text(rel+'\n')
    for name,(rel,_) in FILES.items():replace(source/name,root/rel)
    os.sync();print('已安装 QCA6490 BlueTool 服务、bluetoothd 类型0适配和 RenoirBTType0.dylib（原件在 /.renoir-bluetool-backup）。')
def restore(root):
    backup=root/'.renoir-bluetool-backup'
    for name,(rel,original) in FILES.items():
        saved=backup/name
        if not saved.exists():continue
        if sha(saved)!=original:raise ValueError(f'{name}: original backup hash differs')
        replace(saved,root/rel)
    for name,rel in ADDED.items():
        marker=backup/(name+'.installed')
        if marker.exists():
            (root/rel).unlink(missing_ok=True);marker.unlink()
    os.sync();print('已恢复原版 BlueTool 和 bluetoothd，并移除 RenoirBTType0.dylib；原件备份保留。')
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--root',type=Path,required=True)
    p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]/'build/userland')
    p.add_argument('--restore',action='store_true');a=p.parse_args();root=a.root.resolve()
    if root==Path('/') or not root.is_dir():p.error('expected an offline iOS root')
    version=plistlib.loads((root/'System/Library/CoreServices/SystemVersion.plist').read_bytes())
    if version.get('ProductBuildVersion')!='20H392':p.error('only audited 20H392 supported')
    if os.geteuid()!=0:p.error('sudo required for correct file ownership')
    if os.statvfs(root).f_flag&os.ST_RDONLY:p.error('offline root is read-only')
    if a.restore:restore(root)
    else:install(root,a.source)
if __name__=='__main__':main()
