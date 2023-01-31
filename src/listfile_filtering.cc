#include "listfile_filtering.h"

#include <globals.h>
#include <mesytec-mvlc/mvlc_listfile_gen.h>
#include <mesytec-mvlc/mvlc_listfile_util.h>
#include <mesytec-mvlc/mvlc_listfile_zip.h>
#include <mesytec-mvlc/util/logging.h>
#include <QDebug>

#include "analysis/analysis.h"
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

    std::shared_ptr<spdlog::logger> logger_;
    Logger qtLogger_;
    RunInfo runInfo_;
    const analysis::Analysis *analysis_ = nullptr; // FIXME: make this a shared_ptr or something at some point :-(
    std::unique_ptr<listfile::SplitZipCreator> mvlcZipCreator_;
    std::shared_ptr<listfile::WriteHandle> listfileWriteHandle_;
    ReadoutBuffer outputBuffer_;

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
}

ListfileFilterStreamConsumer::~ListfileFilterStreamConsumer()
{
    d->logger_->debug("destroying @{]", fmt::ptr(this));
}

void ListfileFilterStreamConsumer::setLogger(Logger logger)
{
    d->qtLogger_ = logger;
}

StreamConsumerBase::Logger &ListfileFilterStreamConsumer::getLogger()
{
    return d->qtLogger_;
}

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

void ListfileFilterStreamConsumer::beginRun(const RunInfo &runInfo, const VMEConfig *vmeConfig, const analysis::Analysis *analysis)
{
    d->logger_->debug("@{}: beginRun", fmt::ptr(this));

    // Note: cannot write a mvlc CrateConfig to the output stream as the input
    // may come from a VMUSB or SIS controlle in which case the VMEConfig to
    // CrateConfig conversion is undefined.
    // TODO: check what happens if trying to convert a non MVLC VMEConfig to CrateConfig
    // Also the fact that the config and the listfile have been converted needs
    // to be recorded somewhere and shown to the user.
    auto make_listfile_preamble = [&vmeConfig]() -> std::vector<u8>
    {
        listfile::BufferedWriteHandle bwh;
        listfile::listfile_write_magic(bwh, ConnectionType::USB);
        listfile::listfile_write_endian_marker(bwh);
        mvme_mvlc_listfile::listfile_write_mvme_config(bwh, *vmeConfig);
        return bwh.getBuffer();
    };

    d->runInfo_ = runInfo;
    d->analysis_ = analysis;

    qDebug() << runInfo.runId;
    printMe(runInfo.infoDict);

    auto workspaceSettings = make_workspace_settings();

    // for make_new_listfile_name()
    ListFileOutputInfo lfOutInfo{};
    lfOutInfo.format = ListFileFormat::ZIP;
    // Not actually computing the absolute path here. Should still work as we are inside the workspace directory.
    lfOutInfo.fullDirectory = workspaceSettings.value("ListFileDirectory").toString();
    lfOutInfo.prefix = QFileInfo(runInfo.infoDict["replaySourceFile"].toString()).completeBaseName() + "_filtered";

    QFileInfo lfFileInfo(make_new_listfile_name(&lfOutInfo));
    auto lfDir = lfFileInfo.path();
    auto lfBase = lfFileInfo.completeBaseName();
    auto lfPrefix = lfDir + "/" + lfBase;

    listfile::SplitListfileSetup lfSetup;
    lfSetup.entryType = listfile::ZipEntryInfo::ZIP;
    lfSetup.compressLevel = 1;
    lfSetup.filenamePrefix = lfPrefix.toStdString();
    lfSetup.preamble = make_listfile_preamble();
    // TODO: set lfSetup.closeArchiveCallback to add additional files to the output archive.
    // Instead of using the callback the work could also be done in endRun()
    // _if_ archive splitting is not used.
    // Is splitting a use case for listfile filtering? Could split up a
    // monolithic file based on size or duration or maybe even allow the
    // splitting decision based on data from the analysis.

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

    // FIXME: duplicate in mvlc_readout_worker.cc
    auto do_write = [&zipCreator = d->mvlcZipCreator_] (const std::string &filename, const QByteArray &data)
    {
        auto writeHandle = zipCreator->createZIPEntry(filename, 0); // uncompressed zip entry
        writeHandle->write(reinterpret_cast<const u8 *>(data.data()), data.size());
        zipCreator->closeCurrentEntry();
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
    // TODO: Use a writer thread like in mesytec::mvlc::MVLCReadoutWorker
    if (const auto used = outputBuffer_.used();
        used >= Private::OutputBufferFlushSize)
    {
        logger_->debug("@{}: flushing output buffer, used={}, capacity={}", fmt::ptr(this), used, outputBuffer_.capacity());
        listfileWriteHandle_->write(outputBuffer_.data(), outputBuffer_.used());
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
    listfile::write_event_data(d->outputBuffer_, crateIndex, eventIndex, moduleDataList, moduleCount);
    d->maybeFlushOutputBuffer();
}

void ListfileFilterStreamConsumer::processSystemEvent(s32 crateIndex, const u32 *header, u32 size)
{
    listfile::write_system_event(d->outputBuffer_, crateIndex, header, size);
    d->maybeFlushOutputBuffer();
}

void ListfileFilterStreamConsumer::processModuleData(s32 eventIndex, s32 moduleIndex, const u32 *data, u32 size)
{
    assert(!"don't call me please!");
    throw std::runtime_error(fmt::format("{}: don't call me please!", __PRETTY_FUNCTION__));
}
