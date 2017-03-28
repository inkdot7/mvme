#include "mvme_config.h"
#include "CVMUSBReadoutList.h"

#include <cmath>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>

static QJsonObject storeDynamicProperties(const QObject *object)
{
    QJsonObject json;

    for (auto name: object->dynamicPropertyNames())
       json[QString::fromLocal8Bit(name)] = QJsonValue::fromVariant(object->property(name.constData()));

    return json;
}

static void loadDynamicProperties(const QJsonObject &json, QObject *dest)
{
    auto properties = json.toVariantMap();

    for (auto propName: properties.keys())
    {
        const auto &value = properties[propName];
        dest->setProperty(propName.toLocal8Bit().constData(), value);
    }
}

//
// ConfigObject
//
ConfigObject::ConfigObject(QObject *parent, bool watchDynamicProperties)
    : ConfigObject(parent)
{
    if (watchDynamicProperties)
        setWatchDynamicProperties(true);
}

ConfigObject::ConfigObject(QObject *parent)
    : QObject(parent)
    , m_id(QUuid::createUuid())
{
    connect(this, &QObject::objectNameChanged, this, [this] {
        setModified(true);
    });

    connect(this, &ConfigObject::enabledChanged, this, [this] {
        setModified(true);
    });
}

void ConfigObject::setModified(bool b)
{
    emit modified(b);

    if (m_modified != b)
    {
        qDebug() << __PRETTY_FUNCTION__ << this << m_modified << "->" << b;
        m_modified = b;
        emit modifiedChanged(b);

    }

    if (b)
    {
        if (auto parentConfig = qobject_cast<ConfigObject *>(parent()))
            parentConfig->setModified(true);
    }
}

void ConfigObject::setEnabled(bool b)
{
    if (m_enabled != b)
    {
        m_enabled = b;
        emit enabledChanged(b);
    }
}

QString ConfigObject::getObjectPath() const
{
    if (objectName().isEmpty())
        return QString();

    auto parentConfig = qobject_cast<ConfigObject *>(parent());

    if (!parentConfig)
        return objectName();

    auto result = parentConfig->getObjectPath();

    if (!result.isEmpty())
        result += QChar('/');

    result += objectName();

    return result;
}

void ConfigObject::read(const QJsonObject &json)
{
    m_id = QUuid(json["id"].toString());
    if (m_id.isNull())
        m_id = QUuid::createUuid();

    setObjectName(json["name"].toString());
    setEnabled(json["enabled"].toBool(true));

    read_impl(json);

    setModified(false);
}

void ConfigObject::write(QJsonObject &json) const
{
    json["id"]   = m_id.toString();
    json["name"] = objectName();
    json["enabled"] = m_enabled;

    write_impl(json);
}

bool ConfigObject::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == this && event->type() == QEvent::DynamicPropertyChange)
        setModified();
    return QObject::eventFilter(obj, event);
}

void ConfigObject::setWatchDynamicProperties(bool doWatch)
{
    if (doWatch && !m_eventFilterInstalled)
    {
        installEventFilter(this);
        m_eventFilterInstalled = true;
    }
    else if (!doWatch && m_eventFilterInstalled)
    {
        removeEventFilter(this);
        m_eventFilterInstalled = false;
    }
}

//
// VMEScriptConfig
//
void VMEScriptConfig::setScriptContents(const QString &str)
{
    if (m_script != str)
    {
        m_script = str;
        setModified(true);
    }
}

vme_script::VMEScript VMEScriptConfig::getScript(u32 baseAddress) const
{
    auto script = vme_script::parse(m_script, baseAddress);
    return script;
}

void VMEScriptConfig::read_impl(const QJsonObject &json)
{
    m_script = json["vme_script"].toString();
    loadDynamicProperties(json["properties"].toObject(), this);
}

void VMEScriptConfig::write_impl(QJsonObject &json) const
{
    json["vme_script"] = m_script;
    auto props = storeDynamicProperties(this);
    if (!props.isEmpty())
        json["properties"] = props;
}

QString VMEScriptConfig::getVerboseTitle() const
{
    auto module     = qobject_cast<ModuleConfig *>(parent());
    auto event      = qobject_cast<EventConfig *>(parent());
    auto daqConfig  = qobject_cast<DAQConfig *>(parent());

    QString title;

    if (module)
    {
        title = QString("%1 for %2")
            .arg(objectName())
            .arg(module->objectName());
    }
    else if (event)
    {
        title = QString("%1 for %2")
            .arg(objectName())
            .arg(event->objectName());
    }
    else if (daqConfig)
    {
        title = QString("Global Script %2")
            .arg(objectName());
    }
    else
    {
        title = QString("VMEScript %1")
            .arg(objectName());
    }

    return title;
}

//
// ModuleConfig
//
ModuleConfig::ModuleConfig(QObject *parent)
    : ConfigObject(parent)
{
    vmeScripts["parameters"] = new VMEScriptConfig(this);
    vmeScripts["parameters"]->setObjectName(QSL("Module Init"));

    vmeScripts["readout_settings"] = new VMEScriptConfig(this);
    vmeScripts["readout_settings"]->setObjectName(QSL("VME Interface Settings"));

    vmeScripts["readout"] = new VMEScriptConfig(this);
    vmeScripts["readout"]->setObjectName(QSL("Readout"));

    vmeScripts["reset"] = new VMEScriptConfig(this);
    vmeScripts["reset"]->setObjectName(QSL("Module Reset"));
}

void ModuleConfig::read_impl(const QJsonObject &json)
{
    type = VMEModuleShortNames.key(json["type"].toString(), VMEModuleType::Invalid);
    m_baseAddress = json["baseAddress"].toInt();

    QJsonObject scriptsObject = json["vme_scripts"].toObject();

    for (auto it = scriptsObject.begin();
         it != scriptsObject.end();
         ++it)
    {
        VMEScriptConfig *cfg(new VMEScriptConfig(this));
        cfg->read(it.value().toObject());
        vmeScripts[it.key()] = cfg;
    }
    loadDynamicProperties(json["properties"].toObject(), this);
}

void ModuleConfig::write_impl(QJsonObject &json) const
{
    json["type"] = VMEModuleShortNames.value(type, "invalid");
    json["baseAddress"] = static_cast<qint64>(m_baseAddress);

    QJsonObject scriptsObject;

    for (auto it = vmeScripts.begin();
         it != vmeScripts.end();
         ++it)
    {
        QJsonObject scriptJson;
        if (it.value())
        {
            it.value()->write(scriptJson);
            scriptsObject[it.key()] = scriptJson;
        }
    }

    json["vme_scripts"] = scriptsObject;
    auto props = storeDynamicProperties(this);
    if (!props.isEmpty())
        json["properties"] = props;
}

//
// EventConfig
//

EventConfig::EventConfig(QObject *parent)
    : ConfigObject(parent)
{
    vmeScripts[QSL("daq_start")] = new VMEScriptConfig(this);
    vmeScripts[QSL("daq_start")]->setObjectName(QSL("DAQ Start"));

    vmeScripts[QSL("daq_stop")] = new VMEScriptConfig(this);
    vmeScripts[QSL("daq_stop")]->setObjectName(QSL("DAQ Stop"));

    vmeScripts[QSL("readout_start")] = new VMEScriptConfig(this);
    vmeScripts[QSL("readout_start")]->setObjectName(QSL("Cycle Start"));

    vmeScripts[QSL("readout_end")] = new VMEScriptConfig(this);
    vmeScripts[QSL("readout_end")]->setObjectName(QSL("Cycle End"));
}

void EventConfig::read_impl(const QJsonObject &json)
{
    qDeleteAll(modules);
    modules.clear();

    triggerCondition = static_cast<TriggerCondition>(json["triggerCondition"].toInt());
    irqLevel = json["irqLevel"].toInt();
    irqVector = json["irqVector"].toInt();
    scalerReadoutPeriod = json["scalerReadoutPeriod"].toInt();
    scalerReadoutFrequency = json["scalerReadoutFrequency"].toInt();

    QJsonArray moduleArray = json["modules"].toArray();
    for (int i=0; i<moduleArray.size(); ++i)
    {
        QJsonObject moduleObject = moduleArray[i].toObject();
        ModuleConfig *moduleConfig = new ModuleConfig(this);
        moduleConfig->read(moduleObject);
        modules.append(moduleConfig);
    }


    for (auto scriptConfig: vmeScripts.values())
    {
        scriptConfig->setScriptContents(QString());
    }

    QJsonObject scriptsObject = json["vme_scripts"].toObject();

    for (auto it = scriptsObject.begin();
         it != scriptsObject.end();
         ++it)
    {
        if (vmeScripts.contains(it.key()))
        {
            vmeScripts[it.key()]->read(it.value().toObject());
        }
    }

    loadDynamicProperties(json["properties"].toObject(), this);
}

void EventConfig::write_impl(QJsonObject &json) const
{
    json["triggerCondition"] = static_cast<int>(triggerCondition);
    json["irqLevel"] = irqLevel;
    json["irqVector"] = irqVector;
    json["scalerReadoutPeriod"] = scalerReadoutPeriod;
    json["scalerReadoutFrequency"] = scalerReadoutFrequency;

    QJsonArray moduleArray;

    for (auto module: modules)
    {
        QJsonObject moduleObject;
        module->write(moduleObject);
        moduleArray.append(moduleObject);
    }
    json["modules"] = moduleArray;

    QJsonObject scriptsObject;

    for (auto it = vmeScripts.begin();
         it != vmeScripts.end();
         ++it)
    {
        QJsonObject scriptJson;
        if (it.value())
        {
            it.value()->write(scriptJson);
            scriptsObject[it.key()] = scriptJson;
        }
    }

    json["vme_scripts"] = scriptsObject;
    auto props = storeDynamicProperties(this);
    if (!props.isEmpty())
        json["properties"] = props;
}


//
// DAQConfig
//
// Versioning of the DAQ config in case incompatible changes need to be made.
static const int DAQConfigVersion = 1;

DAQConfig::DAQConfig(QObject *parent)
    : ConfigObject(parent)
{
    setProperty("version", DAQConfigVersion);
}

void DAQConfig::addEventConfig(EventConfig *config)
{
    config->setParent(this);
    eventConfigs.push_back(config);
    emit eventAdded(config);
    setModified();
}

bool DAQConfig::removeEventConfig(EventConfig *config)
{
    bool ret = eventConfigs.removeOne(config);
    if (ret)
    {
        emit eventAboutToBeRemoved(config);
        config->setParent(nullptr);
        config->deleteLater();
        setModified();
    }

    return ret;
}

bool DAQConfig::contains(EventConfig *config)
{
    return eventConfigs.indexOf(config) >= 0;
}

void DAQConfig::addGlobalScript(VMEScriptConfig *config, const QString &category)
{
    config->setParent(this);
    vmeScriptLists[category].push_back(config);
    emit globalScriptAdded(config, category);
    setModified();
}

bool DAQConfig::removeGlobalScript(VMEScriptConfig *config)
{
    for (auto category: vmeScriptLists.keys())
    {
        if (vmeScriptLists[category].removeOne(config))
        {
            emit globalScriptAboutToBeRemoved(config);
            config->setParent(nullptr);
            config->deleteLater();
            setModified();
            return true;
        }
    }

    return false;
}

void DAQConfig::read_impl(const QJsonObject &json)
{
    qDeleteAll(eventConfigs);
    eventConfigs.clear();

    QJsonArray eventArray = json["events"].toArray();

    for (int eventIndex=0; eventIndex<eventArray.size(); ++eventIndex)
    {
        QJsonObject eventObject = eventArray[eventIndex].toObject();
        EventConfig *eventConfig = new EventConfig(this);
        eventConfig->read(eventObject);
        eventConfigs.append(eventConfig);
    }
    qDebug() << __PRETTY_FUNCTION__ << "read" << eventConfigs.size() << "event configs";

    QJsonObject scriptsObject = json["vme_script_lists"].toObject();

    for (auto it = scriptsObject.begin();
         it != scriptsObject.end();
         ++it)
    {
        auto &list(vmeScriptLists[it.key()]);

        QJsonArray scriptsArray = it.value().toArray();

        for (auto arrayIter = scriptsArray.begin();
             arrayIter != scriptsArray.end();
             ++arrayIter)
        {
            VMEScriptConfig *cfg(new VMEScriptConfig(this));
            cfg->read((*arrayIter).toObject());
            list.push_back(cfg);
        }
    }

    loadDynamicProperties(json["properties"].toObject(), this);
}

void DAQConfig::write_impl(QJsonObject &json) const
{
    QJsonArray eventArray;
    for (auto event: eventConfigs)
    {
        QJsonObject eventObject;
        event->write(eventObject);
        eventArray.append(eventObject);
    }
    json["events"] = eventArray;

    QJsonObject scriptsObject;

    for (auto mapIter = vmeScriptLists.begin();
         mapIter != vmeScriptLists.end();
         ++mapIter)
    {
        const auto list(mapIter.value());

        QJsonArray scriptsArray;

        for (auto listIter = list.begin();
             listIter != list.end();
             ++listIter)
        {
            QJsonObject scriptsObject;
            (*listIter)->write(scriptsObject);
            scriptsArray.append(scriptsObject);
        }

        scriptsObject[mapIter.key()] = scriptsArray;
    }

    json["vme_script_lists"] = scriptsObject;
    auto props = storeDynamicProperties(this);
    if (!props.isEmpty())
        json["properties"] = props;
}

ModuleConfig *DAQConfig::getModuleConfig(int eventIndex, int moduleIndex)
{
    ModuleConfig *result = 0;
    auto eventConfig = eventConfigs.value(eventIndex);

    if (eventConfig)
    {
        result = eventConfig->modules.value(moduleIndex);
    }

    return result;
}

EventConfig *DAQConfig::getEventConfig(const QString &name) const
{
    for (auto cfg: eventConfigs)
    {
        if (cfg->objectName() == name)
            return cfg;
    }
    return nullptr;
}

EventConfig *DAQConfig::getEventConfig(const QUuid &id) const
{
    for (auto cfg: eventConfigs)
    {
        if (cfg->getId() == id)
            return cfg;
    }
    return nullptr;
}

QList<ModuleConfig *> DAQConfig::getAllModuleConfigs() const
{
    QList<ModuleConfig *> result;

    for (auto eventConfig: eventConfigs)
    {
        for (auto moduleConfig: eventConfig->modules)
        {
            result.push_back(moduleConfig);
        }
    }

    return result;
}

QPair<int, int> DAQConfig::getEventAndModuleIndices(ModuleConfig *cfg) const
{
    for (int eventIndex = 0;
         eventIndex < eventConfigs.size();
         ++eventIndex)
    {
        auto moduleConfigs = eventConfigs[eventIndex]->getModuleConfigs();
        int moduleIndex = moduleConfigs.indexOf(cfg);
        if (moduleIndex >= 0)
            return qMakePair(eventIndex, moduleIndex);
    }

    return qMakePair(-1, -1);
}
