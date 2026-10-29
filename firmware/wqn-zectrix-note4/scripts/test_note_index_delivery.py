#!/usr/bin/env python3
"""Actual Note installers, UI apply branches and result mailbox on host ports.

Controlled cross-lane/result/staging orders; no device scheduler, storage or HIL.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from test_audio_runtime import function
from test_binary_outbox_recovery import ERR,HEAP


def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before')
    parser.add_argument('--mutation',choices=('no-stamp','accept-old-bulk','accept-old-body','keep-pending','activate-old','wrap-generation','no-shared-repair'))
    args=parser.parse_args()
    def source(path):
        return (subprocess.check_output(['git','show',f'{args.before}:firmware/wqn-zectrix-note4/{path}'],cwd=root,text=True)
                if args.before else (root/path).read_text())
    cloud=source('main/note_cloud.cpp');app=source('main/note_app.cpp')
    if args.mutation=='no-stamp':
        old='out->index.delivery_generation = install_generation;';assert cloud.count(old)==2
        cloud=cloud.replace(old,'out->index.delivery_generation = 0;')
    if args.mutation=='wrap-generation':
        old='if (previous == std::numeric_limits<uint64_t>::max()) return 0;';assert cloud.count(old)==1
        cloud=cloud.replace(old,'/* allow generation wrap */')
    if args.mutation=='no-shared-repair':
        start=cloud.index('esp_err_t SyncSingleNotebookPack(')
        assert cloud[start:].count('g_note_index_stale = true;')==2
        cloud=cloud[:start]+cloud[start:].replace('g_note_index_stale = true;','/* omit shared repair marker */')
    helpers=[]
    for sig in ['bool NotePackIndexIsOlder(','bool NotePackIndexDeliveryIsStale(']:
        if sig in app: helpers.append(function(app,sig))
    core=[]
    for sig in ['void InstallNotePackIndex(','void ActivatePendingNotePackIndex(',
                'void ApplyNotePackIndex(','void ApplyNoteBodyFetchResult(']:
        text=function(app,sig)
        if args.mutation=='accept-old-bulk' and sig=='void ApplyNotePackIndex(':
            text=text.replace('if (NotePackIndexDeliveryIsStale(*state, index)) return;',
                              'if (false && NotePackIndexDeliveryIsStale(*state, index)) return;')
        if args.mutation=='accept-old-body' and sig=='void InstallNotePackIndex(':
            text=text.replace('if (NotePackIndexIsOlder(index, state->pack_index)) return;','/* accept stale body */')
        if args.mutation=='keep-pending' and sig=='void InstallNotePackIndex(':
            text=text.replace('state->pending_pack_index_ready = false;','/* retain pending */')
        if args.mutation=='activate-old' and sig=='void ActivatePendingNotePackIndex(':
            text=text.replace('InstallNotePackIndex(state, std::move(pending), "笔记更新已启用");',
                              'state->pack_index = std::move(pending);')
        core.append(text)
    ui=source('main/ui/note_cloud.cpp');start=ui.index('bool ApplyNoteCloudResult(');end=ui.index('    if (result.op == NoteCloudOp::kStartSession)',start)
    ui_apply=ui[start:end]+'    return false;\n}\n'
    mailbox=source('main/ui/cloud_runner.cpp')
    mailbox_code=function(mailbox,'struct DomainResultMailbox {')+';\nDomainResultMailbox g_result_mailbox[kCloudDomainCount];\n'
    mailbox_code+='\n'.join(function(mailbox,sig) for sig in ['void PublishCloudResult(','bool TakeCloudResultToApply(',
                                                            'bool HasCloudResultPending(','void AckCloudResult('])
    fixture=(root/'scripts/testdata/note_index_delivery.cpp').read_text()
    fixture=fixture.replace('@@UI_HELPERS@@','\n'.join(helpers)).replace('@@INSTALL@@',core[0]).replace('@@ACTIVATE@@',core[1])
    fixture=fixture.replace('@@APPLY@@',core[2]).replace('@@BODY_APPLY@@',core[3]).replace('@@UI_APPLY@@',ui_apply).replace('@@MAILBOX@@',mailbox_code)
    merge=function(source('main/note_pack.cpp'),'esp_err_t MergeNotePackManifestDelta(')
    fixture=fixture.replace('@@MERGE@@',merge)
    generation='delivery_generation' in source('main/note_pack.h')
    fixture=fixture.replace('@@HAS_GENERATION@@',str(int(generation)))
    with tempfile.TemporaryDirectory(prefix='wqn-note-delivery-') as directory:
        temp=Path(directory);(temp/'esp_err.h').write_text(ERR);(temp/'esp_heap_caps.h').write_text(HEAP)
        for name in ['note_pack.h','note_cloud.h','note_app.h']:
            (temp/name).write_text(source('main/'+name))
        (temp/'esp_log.h').write_text('#pragma once\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n#define esp_err_to_name(x) "host"\n')
        (temp/'services').mkdir();(temp/'services/sync_service.h').write_text('#pragma once\n#include "device_protocol/v3.h"\nnamespace wqn::services {protocol::v3::RequestMetadata MakeDeviceRequestMetadata();}\n')
        (temp/'note_cloud.cpp').write_text(cloud);(temp/'fixture.cpp').write_text(fixture)
        binary=temp/'test'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
            '-fno-sanitize-recover=all','-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(temp),
            '-I'+str(root/'main'),'-I'+str(root/'components/device_protocol/include'),
            *[f'-DWQN_{name}_SCHEMA_SHA256="host"' for name in ['DEVICE_CONTROL','WORD_STUDY','NOTE_STUDY','PROBLEM_STUDY']],
            str(temp/'fixture.cpp'),'-o',str(binary)],check=True,timeout=60)
        return subprocess.run([str(binary)],timeout=30,env=dict(os.environ,
            ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1')).returncode


if __name__=='__main__':
    raise SystemExit(main())
