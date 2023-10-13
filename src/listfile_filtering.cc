#include "listfile_filtering.h"

#include <globals.h>
#include <mesytec-mvlc/mvlc_listfile_gen.h>
#include <mesytec-mvlc/mvlc_listfile_util.h>
#include <mesytec-mvlc/mvlc_listfile_zip.h>
#include <mesytec-mvlc/util/logging.h>
#include <mesytec-mvlc/util/protected.h>
#include <QDebug>

#include "analysis/analysis.h"
#include "analysis/a2_adapter.h"
#include "mvme_mvlc_listfile.h"
#include "mvme_workspace.h"
#include "run_info.h"
#include "vme_daq.h"

using namespace mesytec::mvlc;

namespace
{
    // https://stackoverflow.com/a/1759114/17562886
    template <class NonMap>
    struct Print
    {
        static void print(const QString &tabs, const NonMap &value)
        {
            qDebug() << tabs << value;
        }
    };

    template <class Key, class ValueType>
    struct Print<class QMap<Key, ValueType>>
    {
        static void print(const QString &tabs, const QMap<Key, ValueType> &map)
        {
            const QString extraTab = tabs + "\t";
            QMapIterator<Key, ValueType> iterator(map);
            while (iterator.hasNext())
            {
                iterator.next();
                qDebug() << tabs << iterator.key();
                Print<ValueType>::print(extraTab, iterator.value());
            }
        }
    };

    template <class Type>
    void printMe(const Type &type)
    {
        Print<Type>::print("", type);
    };
}

struct ListfileFilterStreamConsumer::Private
{
    static const size_t OutputBufferInitialCapacity = mesytec::mvlc::util::Megabytes(1);
    static const size_t OutputBufferFlushSize = OutputBufferInitialCapacity;

    ListfileFilterConfig config_;

    std::shared_ptr<spdlog::logger> logger_;
    Logger qtLogger_;

    RunInfo runInfo_;
    VMEControllerType inputControllerType_;
    // TODO: make this a shared_ptr or something at some point. It's passed in
    // beginRun() and must stay valid during the run.
    const analysis::Analysis *analysis_ = nullptr;
    std::unique_ptr<listfile::SplitZipCreator> mvlcZipCreator_;
    std::shared_ptr<listfile::WriteHandle> listfileWriteHandle_;
    ReadoutBuffer outputBuffer_;
    mutable mesytec::mvlc::Protected<MVMEStreamProcessorCounters> counters_;

    void maybeFlushOutputBuffer();
};

ListfileFilterStreamConsumer::ListfileFilterStreamConsumer()
    : d(std::make_unique<Private>())
{
    d->logger_ = get_logger("ListfileFilterStreamConsumer");
    d->logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [tid%t] %v");
    d->logger_->set_level(spdlog::level::debug); // FIXME: remove this

    d->outputBuffer_ = ReadoutBuffer(Private::OutputBufferInitialCapacity);
    d->logger_->debug("created @{}", fmt::ptr(this));

    d->config_.outputInfo.enabled = false;
}

ListfileFilterStreamConsumer::~ListfileFilterStreamConsumer()
{
    d->logger_->debug("destroying @{}", fmt::ptr(this));
}

void ListfileFilterStreamConsumer::setLogger(Logger logger)
{
    d->qtLogger_ = logger;
}

StreamConsumerBase::Logger &ListfileFilterStreamConsumer::getLogger()
{
    return d->qtLogger_;
}

//void ListfileFilterStreamConsumer::setEnabled(bool b)
//{
//    d->enabled_ = b;
//}

#if 0
[2023-01-30 16:43:07.256] [info] virtual void ListfileFilterStreamConsumer::beginRun(const RunInfo&, const VMEConfig*, const analysis::Analysis*) @ 0x555556868390
"" "ExperimentName"
"\t" QVariant(QString, "Experiment")
"" "ExperimentTitle"
"\t" QVariant(QString, "")
"" "MVMEWorkspace"
"\t" QVariant(QString, "/home/florian/Documents/mvme-workspaces-on-hdd/2301_listfile-filtering")
"" "replaySourceFile"
"\t" QVariant(QString, "00-mdpp_trig.zip")

runId is "00-mdpp_trig"
#endif

void ListfileFilterStreamConsumer::beginRun(
    const RunInfo &runInfo, const VMEConfig *vmeConfig, const analysis::Analysis *analysis)
{
    d->logger_->debug("@{}: beginRun", fmt::ptr(this));

    if (!is_mvlc_controller(vmeConfig->getControllerType()))
    {
        getLogger()("Error: listfile filtering is only implemented for the MVLC controller");
        return;
    }

    if (auto format = d->config_.outputInfo.format;
        format != ListFileFormat::ZIP && format != ListFileFormat::LZ4)
    {
        getLogger()("Error: listfile filter can only output ZIP or LZ4 archives");
        return;
    }

    d->runInfo_ = runInfo;
    d->inputControllerType_ = vmeConfig->getControllerType();
    d->analysis_ = analysis;

    printMe(runInfo.infoDict);

    auto workspaceSettings = make_workspace_settings();

#if 0
    // for make_new_listfile_name()
    ListFileOutputInfo lfOutInfo{};
    lfOutInfo.format = ListFileFormat::ZIP;
    // Not actually computing the absolute path here. Should still work as we are inside the workspace directory.
    lfOutInfo.fullDirectory = workspaceSettings.value("ListFileDirectory").toString();
    lfOutInfo.prefix = QFileInfo(runInfo.infoDict["replaySourceFile"].toString()).completeBaseName() + "_filtered";
#endif


    auto make_listfile_preamble = [&vmeConfig]() -> std::vector<u8>
    {
        listfile::BufferedWriteHandle bwh;
        listfile::listfile_write_magic(bwh, ConnectionType::USB);
        listfile::listfile_write_endian_marker(bwh);
        // FIXME: why write the vme config? it should be streamed via the first
        // system event from the source listfile. But the system event header will
        // not be compatible when the source listfile is non-mvlc! Have to fixup the headers.
        //mvme_mvlc_listfile::listfile_write_mvme_config(bwh, *vmeConfig);
        return bwh.getBuffer();
    };

    const auto &outInfo = d->config_.outputInfo;
    // FIXME: copy of the code in mvlc_readout_worker.cc
    listfile::SplitListfileSetup lfSetup;
    lfSetup.entryType = (outInfo.format == ListFileFormat::ZIP
                            ? listfile::ZipEntryInfo::ZIP
                            : listfile::ZipEntryInfo::LZ4);
    lfSetup.compressLevel = outInfo.compressionLevel;
    if (outInfo.flags & ListFileOutputInfo::SplitBySize)
        lfSetup.splitMode = listfile::ZipSplitMode::SplitBySize;
    else if (outInfo.flags & ListFileOutputInfo::SplitByTime)
        lfSetup.splitMode = listfile::ZipSplitMode::SplitByTime;

    lfSetup.splitSize = outInfo.splitSize;
    lfSetup.splitTime = outInfo.splitTime;

    QFileInfo lfFileInfo(make_new_listfile_name(&d->config_.outputInfo));
    auto lfDir = lfFileInfo.path();
    auto lfBase = lfFileInfo.completeBaseName();
    auto lfPrefix = lfDir + "/" + lfBase;

    lfSetup.filenamePrefix = lfPrefix.toStdString();
    lfSetup.preamble = make_listfile_preamble();
    // FIXME end of code copied from mvlc_readout_worker.cc

    // TODO: set lfSetup.closeArchiveCallback to add additional files to the output archive.

    d->logger_->info("@{}: output filename is {}", fmt::ptr(this), lfSetup.filenamePrefix);

    d->mvlcZipCreator_ = std::make_unique<listfile::SplitZipCreator>();
    d->mvlcZipCreator_->createArchive(lfSetup); // FIXME: does it throw? yes, it probably does
    d->listfileWriteHandle_ = std::shared_ptr<listfile::WriteHandle>(
        d->mvlcZipCreator_->createListfileEntry());
    d->outputBuffer_.clear();

    d->logger_->debug("@{}: beginRun is done, output archive: {}, listfile entry: {}",
        fmt::ptr(this), d->mvlcZipCreator_->archiveName(), d->mvlcZipCreator_->entryInfo().name);
}

void ListfileFilterStreamConsumer::endRun(const DAQStats &stats, const std::exception *e)
{
    d->logger_->debug("@{}: endRun", fmt::ptr(this));
    d->listfileWriteHandle_.reset();
    d->mvlcZipCreator_->closeCurrentEntry();

    auto do_write = [zipCreator = d->mvlcZipCreator_.get()] (const std::string &filename, const QByteArray &data)
    {
        listfile::add_file_to_archive(zipCreator, filename, data);
    };

    // Copy the log buffer from the input archive. TODO: add info that this file
    // was filtered, how much was filtered and when the filtering took place.
    do_write("messages.log",  d->runInfo_.infoDict["listfileLogBuffer"].toByteArray());

    if (d->analysis_)
        do_write("analysis.analysis", analysis::serialize_analysis_to_json_document(*d->analysis_).toJson());

    d->logger_->debug("@{}: endRun is done", fmt::ptr(this));
}

void ListfileFilterStreamConsumer::Private::maybeFlushOutputBuffer()
{
    // TODO: Use a writer thread like in mesytec::mvlc::MVLCReadoutWorker. This
    // way we could return and the next events could be processed by the
    // analysis.
    if (const auto used = outputBuffer_.used();
        used >= Private::OutputBufferFlushSize)
    {
        logger_->debug("@{}: flushing output buffer, used={}, capacity={}", fmt::ptr(this), used, outputBuffer_.capacity());
        listfileWriteHandle_->write(outputBuffer_.data(), used);
        outputBuffer_.clear();
    }
}

void ListfileFilterStreamConsumer::beginEvent(s32 eventIndex)
{
}

void ListfileFilterStreamConsumer::endEvent(s32 eventIndex)
{
}

void ListfileFilterStreamConsumer::processModuleData(
    s32 crateIndex, s32 eventIndex, const ModuleData *moduleDataList, unsigned moduleCount)
{
    if (eventIndex < static_cast<s32>(d->config_.filterConditionsByEvent.size()))
    {
        // TODO: to improve performance build a vector of condition bit indexes
        // per event which will save doing the A2AdapterState hash lookups.
        // => condValue = a2->conditionBits.test(d->eventConditionIndexes[eventIndex])

        auto conditionBitIndexes = d->analysis_->getA2AdapterState()->conditionBitIndexes;
        auto condId = d->config_.filterConditionsByEvent[eventIndex];
        if (auto a1_cond = d->analysis_->getObject<analysis::ConditionInterface>(condId))
        {
            if (auto bitIndex = conditionBitIndexes.value(a1_cond.get(), -1);
                bitIndex >= 0)
            {
                auto condValue = d->analysis_->getA2AdapterState()->a2->conditionBits.test(bitIndex);

                if (!condValue)
                {
                    // TODO: count the filtered out event
                    return;
                }
            }
        }
    }

    listfile::write_event_data(d->outputBuffer_, crateIndex, eventIndex, moduleDataList, moduleCount);
    d->maybeFlushOutputBuffer();
}

void ListfileFilterStreamConsumer::processSystemEvent(s32 crateIndex, const u32 *header, u32 size)
{
    // Note: works for MVLC inputs only, as otherwise the system event header won't be compatible.
    listfile::write_system_event(d->outputBuffer_, crateIndex, header, size);
    d->maybeFlushOutputBuffer();
}

void ListfileFilterStreamConsumer::processModuleData(s32 eventIndex, s32 moduleIndex, const u32 *data, u32 size)
{
    assert(!"don't call me please!");
    throw std::runtime_error(fmt::format("{}: don't call me please!", __PRETTY_FUNCTION__));
}

MVMEStreamProcessorCounters ListfileFilterStreamConsumer::getCounters() const
{
    return d->counters_.copy();
}

void ListfileFilterStreamConsumer::setConfig(const ListfileFilterConfig &config)
{
    d->config_ = config;
}
