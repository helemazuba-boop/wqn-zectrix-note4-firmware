#!/usr/bin/env python3
"""Actual upload loop + full Word store/protocol on libc; SDK/storage worker are ports."""
import argparse
from pathlib import Path
import re
from test_audio_runtime import function
from test_sync_retry import section
import test_word_local_descriptor as native


def main():
    root=Path(__file__).resolve().parents[1]
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sync-source',type=Path,default=root/'main/services/sync_service.cpp')
    p.add_argument('--store-source',type=Path,default=root/'main/word_study_store.cpp')
    p.add_argument('--mutation',choices=('small-peek','oversized-ack','no-read-backoff'))
    a=p.parse_args();sync=a.sync_source.read_text();store=a.store_source.read_text()
    upload=section(sync,'WordOutboxUploadState UploadPendingWordObservations(','// Uploads pending problem verdicts')
    if a.mutation=='oversized-ack':
        old='const size_t count = std::min(acknowledgements.size(), wqn::kWordOutboxJournalBatchCapacity);'
        assert upload.count(old)==1;upload=upload.replace(old,'const size_t count = acknowledgements.size();')
    if a.mutation=='no-read-backoff':
        old='ScheduleWordOutboxRetry(pending.request_id, 0, OutboxRetryCause::kLocalStorage);\n                    return WordOutboxUploadState::kPending;'
        assert upload.count(old)==1;upload=upload.replace(old,'return WordOutboxUploadState::kPending;')
    if a.mutation=='small-peek':
        old='context->excluded->size() > wqn::kWordOutboxReadExclusionCapacity'
        assert store.count(old)==1;store=store.replace(old,'context->excluded->size() > kOutboxAppendBatchCapacity')
    ports=(root/'scripts/testdata/word_local_descriptor.cpp').read_text().split('// ---- test helpers')[0]
    marker='namespace wqn::services {'
    ports=ports.replace(marker,'esp_err_t RunFixtureTransaction(wqn::services::StorageTransaction,void*,const char*);\n'+marker,1)
    ports=ports.replace('StorageTransaction tx, void* ctx, const char*)','StorageTransaction tx, void* ctx, const char* owner)')
    assert ports.count('return tx ? tx(ctx) : ESP_ERR_INVALID_ARG;')==2
    ports=ports.replace('return tx ? tx(ctx) : ESP_ERR_INVALID_ARG;','return RunFixtureTransaction(tx,ctx,owner);')
    ports=ports.replace('const char*, int64_t)','const char* owner, int64_t)')
    ports=ports.replace('esp_err_t r = tx ? tx(ctx) : ESP_ERR_INVALID_ARG;','esp_err_t r = RunFixtureTransaction(tx,ctx,owner);')
    all_adapter=(root/'scripts/testdata/word_ack_upload.cpp').read_text()
    adapter=all_adapter.split('namespace wqn {\nesp_err_t PeekPendingWordObservationExcluding')[0]
    adapter+='\nnamespace wqn {\n'+function(all_adapter,'esp_err_t SubmitWordStudyObservationV1(')+'\n'+function(all_adapter,'esp_err_t SkipWordStudyObservationV1(')+'\n}\n'
    adapter=re.sub(r'const char\* esp_err_to_name\(int\).*?\n','',adapter)
    adapter=re.sub(r'#define ESP_LOG[IWED].*?\n','',adapter)
    adapter=adapter.replace('namespace wqn {\nusing Metadata = protocol::v3::RequestMetadata;\nstruct LocalWordSessionDescriptor { PersistedWordSession session; uint32_t total_count=0; };\n}',
                            'namespace wqn { using Metadata = protocol::v3::RequestMetadata; }')
    for signature in ('esp_err_t LoadLocalWordSessionDescriptor(', 'void ResetWordOutboxRetryBackoff(',
                      'bool WordOutboxRetryDeferred(', 'void ScheduleWordOutboxRetry('):
        adapter=adapter.replace(function(adapter,signature),'')
    adapter=adapter.replace('enum class OutboxRetryCause { kLocalStorage,kServer };','enum class OutboxRetryCause { kNone,kTransport,kLocalStorage,kServer };')
    adapter=adapter.replace('constexpr size_t kMaxWordObservationBatchSize=64;',next(n for n in sync.splitlines() if n.startswith('constexpr size_t kMaxWordObservationBatchSize =')))
    retry=section(sync,'void ResetWordOutboxRetryBackoff()','\nvoid ResetNoteOutboxRetryBackoff()')
    constants='\n'.join(n for n in sync.splitlines() if n.startswith(('constexpr uint32_t kWordOutboxRetry','constexpr uint8_t kWordOutboxRetryMaxShift')))
    scheduler=function(sync,'TickType_t OutboxWaitDelay(')
    body=(root/'scripts/testdata/word_upload_store.cpp').read_text()
    fixture=ports+'\n'+adapter+'\n'+body.replace('@@RETRY_CONSTANTS@@',constants).replace('@@RETRY@@',retry).replace('@@WAIT@@',scheduler).replace('@@UPLOAD@@',upload)
    # The reusable builder owns its temp directory and compiles the full store.
    import tempfile
    with tempfile.TemporaryDirectory(prefix='wqn-upload-store-source-') as temp:
        selected=Path(temp)/'store.cpp';selected.write_text(store)
        return native.main(fixture,source=selected,real_protocol=True)


if __name__=='__main__':raise SystemExit(main())
