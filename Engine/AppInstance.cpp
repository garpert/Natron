//  Natron
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/*
 * Created by Alexandre GAUTHIER-FOICHAT on 6/1/2012.
 * contact: immarespond at gmail dot com
 *
 */

// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>

#include "AppInstance.h"

#include <fstream>
#include <list>
#include <stdexcept>

#include <QDir>
#include <QtConcurrentMap>
#include <QThreadPool>
#include <QUrl>
#include <QFileInfo>
#include <QEventLoop>
#include <QSettings>

#if !defined(SBK_RUN) && !defined(Q_MOC_RUN)
#include <boost/bind.hpp>
#endif

#include "Global/QtCompat.h"

#include "Engine/Project.h"
#include "Engine/Plugin.h"
#include "Engine/AppManager.h"
#include "Engine/Node.h"
#include "Engine/ViewerInstance.h"
#include "Engine/BlockingBackgroundRender.h"
#include "Engine/NodeSerialization.h"
#include "Engine/FileDownloader.h"
#include "Engine/Settings.h"
#include "Engine/KnobTypes.h"
#include "Engine/NoOp.h"
#include "Engine/OfxHost.h"

using namespace Natron;

class FlagSetter {
    
    bool* p;
    QMutex* lock;
    
public:
    
    FlagSetter(bool initialValue,bool* p)
    : p(p)
    , lock(0)
    {
        *p = initialValue;
    }
    
    FlagSetter(bool initialValue,bool* p, QMutex* mutex)
    : p(p)
    , lock(mutex)
    {
        lock->lock();
        *p = initialValue;
        lock->unlock();
    }
    
    ~FlagSetter()
    {
        if (lock) {
            lock->lock();
        }
        *p = !*p;
        if (lock) {
            lock->unlock();
        }
    }
};


struct AppInstancePrivate
{
    boost::shared_ptr<Natron::Project> _currentProject; //< ptr to the project
    int _appID; //< the unique ID of this instance (or window)
    bool _projectCreatedWithLowerCaseIDs;
    
    mutable QMutex creatingGroupMutex;
    bool _creatingGroup;
    bool _creatingNode;
    
    AppInstancePrivate(int appID,
                       AppInstance* app)
    : _currentProject( new Natron::Project(app) )
    , _appID(appID)
    , _projectCreatedWithLowerCaseIDs(false)
    , creatingGroupMutex()
    , _creatingGroup(false)
    , _creatingNode(false)
    {
    }
    
    void declareCurrentAppVariable_Python();
    
};

AppInstance::AppInstance(int appID)
    : QObject()
      , _imp( new AppInstancePrivate(appID,this) )
{
    appPTR->registerAppInstance(this);
    appPTR->setAsTopLevelInstance(appID);


    ///initialize the knobs of the project before loading anything else.
    _imp->_currentProject->initializeKnobsPublic();
    
}

AppInstance::~AppInstance()
{
    appPTR->removeInstance(_imp->_appID);

    ///Clear nodes now, not in the destructor of the project as
    ///deleting nodes might reference the project.
    _imp->_currentProject->clearNodes(false);
    _imp->_currentProject->discardAppPointer();
}

void
AppInstance::setCreatingNode(bool b)
{
    QMutexLocker k(&_imp->creatingGroupMutex);
    _imp->_creatingNode = b;
}

bool
AppInstance::isCreatingNode() const
{
    QMutexLocker k(&_imp->creatingGroupMutex);
    return _imp->_creatingNode;
}

void
AppInstance::checkForNewVersion() const
{
    FileDownloader* downloader = new FileDownloader( QUrl(NATRON_LAST_VERSION_URL) );
    QObject::connect( downloader, SIGNAL( downloaded() ), this, SLOT( newVersionCheckDownloaded() ) );
    QObject::connect( downloader, SIGNAL( error() ), this, SLOT( newVersionCheckError() ) );

    ///make the call blocking
    QEventLoop loop;

    connect( downloader->getReply(), SIGNAL( finished() ), &loop, SLOT( quit() ) );
    loop.exec();
}

//return -1 if a < b, 0 if a == b and 1 if a > b
//Returns -2 if not understood
int compareDevStatus(const QString& a,const QString& b)
{
    if (a == NATRON_DEVELOPMENT_ALPHA) {
        if (b == NATRON_DEVELOPMENT_ALPHA) {
            return 0;
        } else {
            return -1;
        }
    } else if (a == NATRON_DEVELOPMENT_BETA) {
        if (b == NATRON_DEVELOPMENT_ALPHA) {
            return 1;
        } else if (b == NATRON_DEVELOPMENT_BETA) {
            return 0;
        } else {
            return -1;
        }
    } else if (a == NATRON_DEVELOPMENT_RELEASE_CANDIDATE) {
        if (b == NATRON_DEVELOPMENT_ALPHA) {
            return 1;
        } else if (b == NATRON_DEVELOPMENT_BETA) {
            return 1;
        } else if (b == NATRON_DEVELOPMENT_RELEASE_CANDIDATE) {
            return 0;
        } else {
            return -1;
        }
    } else if (a == NATRON_DEVELOPMENT_RELEASE_STABLE) {
        if (b == NATRON_DEVELOPMENT_RELEASE_STABLE) {
            return 0;
        } else {
            return 1;
        }
    }
    assert(false);
    return -2;
}

void
AppInstance::newVersionCheckDownloaded()
{
    FileDownloader* downloader = qobject_cast<FileDownloader*>( sender() );
    assert(downloader);
    
    QString extractedFileVersionStr,extractedSoftwareVersionStr,extractedDevStatusStr,extractedBuildNumberStr;
    
    QString fileVersionTag("File version: ");
    QString softwareVersionTag("Software version: ");
    QString devStatusTag("Development status: ");
    QString buildNumberTag("Build number: ");
    
    QString data( downloader->downloadedData() );
    QTextStream ts(&data);
    
    while (!ts.atEnd()) {
        QString line = ts.readLine();
        if (line.startsWith(QChar('#')) || line.startsWith(QChar('\n'))) {
            continue;
        }
        
        if (line.startsWith(fileVersionTag)) {
            int i = fileVersionTag.size();
            while ( i < line.size() && !line.at(i).isSpace()) {
                extractedFileVersionStr.push_back( line.at(i) );
                ++i;
            }
        } else if (line.startsWith(softwareVersionTag)) {
            int i = softwareVersionTag.size();
            while ( i < line.size() && !line.at(i).isSpace()) {
                extractedSoftwareVersionStr.push_back( line.at(i) );
                ++i;
            }
        } else if (line.startsWith(devStatusTag)) {
            int i = devStatusTag.size();
            while ( i < line.size() && !line.at(i).isSpace()) {
            extractedDevStatusStr.push_back( line.at(i) );
                ++i;
            }
        } else if (line.startsWith(buildNumberTag)) {
            int i = buildNumberTag.size();
            while ( i < line.size() && !line.at(i).isSpace()) {
                extractedBuildNumberStr.push_back( line.at(i) );
                ++i;
            }

        }
    }
    
    if (extractedFileVersionStr.isEmpty() || extractedFileVersionStr.toInt() < NATRON_LAST_VERSION_FILE_VERSION) {
        //The file cannot be decoded here
        downloader->deleteLater();
        return;
    }
    
    
    
    QStringList versionDigits = extractedSoftwareVersionStr.split( QChar('.') );

    ///we only understand 3 digits formed version numbers
    if (versionDigits.size() != 3) {
        downloader->deleteLater();
        return;
    }
    
    int buildNumber = extractedBuildNumberStr.toInt();

    int major = versionDigits[0].toInt();
    int minor = versionDigits[1].toInt();
    int revision = versionDigits[2].toInt();
    
    int devStatCompare = compareDevStatus(extractedDevStatusStr, QString(NATRON_DEVELOPMENT_STATUS));
    
    int versionEncoded = NATRON_VERSION_ENCODE(major, minor, revision);
    if (versionEncoded > NATRON_VERSION_ENCODED ||
        (versionEncoded == NATRON_VERSION_ENCODED &&
         (devStatCompare > 0 || (devStatCompare == 0 && buildNumber > NATRON_BUILD_NUMBER)))) {
            
            QString text;
            if (devStatCompare == 0 && buildNumber > NATRON_BUILD_NUMBER && versionEncoded == NATRON_VERSION_ENCODED) {
                ///show build number in version
                text =  QObject::tr("<p>Updates for %1 are now available for download. "
                                    "You are currently using %1 version %2 - %3 - build %4. "
                                    "The latest version of %1 is version %5 - %6 - build %7.</p> ")
                .arg(NATRON_APPLICATION_NAME)
                .arg(NATRON_VERSION_STRING)
                .arg(NATRON_DEVELOPMENT_STATUS)
                .arg(NATRON_BUILD_NUMBER)
                .arg(extractedSoftwareVersionStr)
                .arg(extractedDevStatusStr)
                .arg(extractedBuildNumberStr) +
                QObject::tr("<p>You can download it from ") + QString("<a href='http://sourceforge.net/projects/natron/'>"
                                                                     "<font color=\"orange\">Sourceforge</a>. </p>");
            } else {
                text =  QObject::tr("<p>Updates for %1 are now available for download. "
                                    "You are currently using %1 version %2 - %3. "
                                    "The latest version of %1 is version %4 - %5.</p> ")
                .arg(NATRON_APPLICATION_NAME)
                .arg(NATRON_VERSION_STRING)
                .arg(NATRON_DEVELOPMENT_STATUS)
                .arg(extractedSoftwareVersionStr)
                .arg(extractedDevStatusStr) +
                QObject::tr("<p>You can download it from ") + QString("<a href='http://sourceforge.net/projects/natron/'>"
                                                                    "<font color=\"orange\">Sourceforge</a>. </p>");

            }
        
            Natron::informationDialog( "New version",text.toStdString(), true );
    }
    downloader->deleteLater();
}

void
AppInstance::newVersionCheckError()
{
    ///Nothing to do,
    FileDownloader* downloader = qobject_cast<FileDownloader*>( sender() );

    assert(downloader);
    downloader->deleteLater();
}

void
AppInstance::getWritersWorkForCL(const CLArgs& cl,std::list<AppInstance::RenderRequest>& requests)
{
    const std::list<CLArgs::WriterArg>& writers = cl.getWriterArgs();
    for (std::list<CLArgs::WriterArg>::const_iterator it = writers.begin(); it != writers.end(); ++it) {
        AppInstance::RenderRequest r;
        
        int firstFrame = INT_MIN,lastFrame = INT_MAX;
        if (cl.hasFrameRange()) {
            const std::pair<int,int>& range = cl.getFrameRange();
            firstFrame = range.first;
            lastFrame = range.second;
        }
        
        if (it->mustCreate) {
            
            NodePtr writer = createWriter(it->filename.toStdString(), getProject(), firstFrame, lastFrame);
            
            //Connect the writer to the corresponding Output node input
            NodePtr output = getProject()->getNodeByFullySpecifiedName(it->name.toStdString());
            if (!output) {
                throw std::invalid_argument(it->name.toStdString() + tr(" is not the name of a valid Output node of the script").toStdString());
            }
            GroupOutput* isGrpOutput = dynamic_cast<GroupOutput*>(output->getLiveInstance());
            if (!isGrpOutput) {
                throw std::invalid_argument(it->name.toStdString() + tr(" is not the name of a valid Output node of the script").toStdString());
            }
            NodePtr outputInput = output->getRealInput(0);
            if (outputInput) {
                writer->connectInput(outputInput, 0);
            }
            
            r.writerName = writer->getScriptName().c_str();
            r.firstFrame = firstFrame;
            r.lastFrame = lastFrame;
        } else {
            r.writerName = it->name;
            r.firstFrame = firstFrame;
            r.lastFrame = lastFrame;
        }
        requests.push_back(r);
    }
}

NodePtr
AppInstance::createWriter(const std::string& filename,
                          const boost::shared_ptr<NodeCollection>& collection,
                          bool userEdited,
                          int firstFrame, int lastFrame)
{
    std::map<std::string,std::string> writersForFormat;
    appPTR->getCurrentSettings()->getFileFormatsForWritingAndWriter(&writersForFormat);
    
    QString fileCpy(filename.c_str());
    std::string ext = Natron::removeFileExtension(fileCpy).toStdString();
    std::map<std::string,std::string>::iterator found = writersForFormat.find(ext);
    if ( found == writersForFormat.end() ) {
        Natron::errorDialog( tr("Writer").toStdString(),
                            tr("No plugin capable of encoding ").toStdString() + ext + tr(" was found.").toStdString(),false );
        return NodePtr();
    }
    
    
    CreateNodeArgs::DefaultValuesList defaultValues;
    defaultValues.push_back(createDefaultValueForParam<std::string>(kOfxImageEffectFileParamName, filename));
    if (firstFrame != INT_MIN && lastFrame != INT_MAX) {
        defaultValues.push_back(createDefaultValueForParam<int>("frameRange", 2));
        defaultValues.push_back(createDefaultValueForParam<int>("firstFrame", firstFrame));
        defaultValues.push_back(createDefaultValueForParam<int>("lastFrame", lastFrame));
    }
    CreateNodeArgs args(found->second.c_str(),
                        "",
                        -1,
                        -1,
                        true,
                        INT_MIN,INT_MIN,
                        true,
                        true,
                        userEdited,
                        QString(),
                        defaultValues,
                        collection);
    return createNode(args);
    
}

void
AppInstance::load(const CLArgs& cl)
{
    
    declareCurrentAppVariable_Python();

    ///if the app is a background project autorun and the project name is empty just throw an exception.
    if ( (appPTR->getAppType() == AppManager::eAppTypeBackgroundAutoRun ||
          appPTR->getAppType() == AppManager::eAppTypeBackgroundAutoRunLaunchedFromGui)) {
        
        if (cl.getFilename().isEmpty()) {
            // cannot start a background process without a file
            throw std::invalid_argument(tr("Project file name empty").toStdString());
        }
        
        
        QFileInfo info(cl.getFilename());
        if (!info.exists()) {
            throw std::invalid_argument(tr("Specified file does not exist").toStdString());
        }
        
        std::list<AppInstance::RenderRequest> writersWork;

        if (info.suffix() == NATRON_PROJECT_FILE_EXT) {
            
            if ( !_imp->_currentProject->loadProject(info.path(),info.fileName()) ) {
                throw std::invalid_argument(tr("Project file loading failed.").toStdString());
            }
            
            getWritersWorkForCL(cl, writersWork);
            startWritersRendering(writersWork);

        } else if (info.suffix() == "py") {
            
            loadPythonScript(info);
            getWritersWorkForCL(cl, writersWork);

        } else {
            throw std::invalid_argument(tr(NATRON_APPLICATION_NAME " only accepts python scripts or .ntp project files").toStdString());
        }
        
        startWritersRendering(writersWork);
        
    } else if (appPTR->getAppType() == AppManager::eAppTypeInterpreter) {
        QFileInfo info(cl.getFilename());
        if (info.exists() && info.suffix() == "py") {
            loadPythonScript(info);
        }
        
        
        appPTR->launchPythonInterpreter();
    } else {
        execOnProjectCreatedCallback();
    }
}

bool
AppInstance::loadPythonScript(const QFileInfo& file)
{
    
    std::string addToPythonPath("sys.path.append(\"");
    addToPythonPath += file.path().toStdString();
    addToPythonPath += "\")\n";
    
    std::string err;
    bool ok  = interpretPythonScript(addToPythonPath, &err, 0);
    assert(ok);

    ok = Natron::interpretPythonScript("app = app1\n", &err, 0);
    assert(ok);
    
    QString filename = file.fileName();
    int lastDotPos = filename.lastIndexOf(QChar('.'));
    if (lastDotPos != -1) {
        filename = filename.left(lastDotPos);
    }
    
    QString hasCreateInstanceScript = QString("import sys\n"
                                              "import %1\n"
                                              "ret = True\n"
                                              "if not hasattr(%1,\"createInstance\") or not hasattr(%1.createInstance,\"__call__\"):\n"
                                              "    ret = False\n").arg(filename);
    
    
    ok = Natron::interpretPythonScript(hasCreateInstanceScript.toStdString(), &err, 0);
    
    if (!ok) {
        Natron::errorDialog(tr("Python").toStdString(), err);
        return false;
    }
    
    
    PyObject* mainModule = getMainModule();
    PyObject* retObj = PyObject_GetAttrString(mainModule,"ret"); //new ref
    assert(retObj);
    bool hasCreateInstance = PyObject_IsTrue(retObj) == 1;
    Py_XDECREF(retObj);
    
    ok = interpretPythonScript("del ret\n", &err, 0);
    assert(ok);
    
    if (hasCreateInstance) {
        std::string output;
        FlagSetter flag(true, &_imp->_creatingGroup, &_imp->creatingGroupMutex);
        if (!Natron::interpretPythonScript(filename.toStdString() + ".createInstance(app,app)", &err, &output)) {
            Natron::errorDialog(tr("Python").toStdString(), err);
            return false;
        } else {
            if (!output.empty()) {
                if (appPTR->isBackground()) {
                    std::cout << output << std::endl;
                } else {
                    appendToScriptEditor(output);
                }
            }
        }
    }
    
    return true;
}

boost::shared_ptr<Natron::Node>
AppInstance::createNodeFromPythonModule(Natron::Plugin* plugin,
                                        const boost::shared_ptr<NodeCollection>& group,
                                        bool requestedByLoad,
                                        const NodeSerialization & serialization)

{
    QString pythonModulePath = plugin->getPythonModule();
    QString moduleName;
    QString modulePath;
    int foundLastSlash = pythonModulePath.lastIndexOf('/');
    if (foundLastSlash != -1) {
        modulePath = pythonModulePath.mid(0,foundLastSlash + 1);
        moduleName = pythonModulePath.remove(0,foundLastSlash + 1);
    }
    
    boost::shared_ptr<Natron::Node> node;
    
    {
        FlagSetter fs(true,&_imp->_creatingGroup,&_imp->creatingGroupMutex);
        
        CreateNodeArgs groupArgs(PLUGINID_NATRON_GROUP,
                                 "",
                                 -1,-1,
                                 true, //< autoconnect
                                 INT_MIN,INT_MIN,
                                 true, //< push undo/redo command
                                 true, // add to project
                                 true,
                                 QString(),
                                 CreateNodeArgs::DefaultValuesList(),
                                 group);
        NodePtr containerNode = createNode(groupArgs);
        if (!containerNode) {
            return containerNode;
        }
        std::string containerName;
        group->initNodeName(plugin->getLabelWithoutSuffix().toStdString(),&containerName);
        containerNode->setScriptName(containerName);
        
        
        if (!requestedByLoad) {
            std::string containerFullySpecifiedName = containerNode->getFullyQualifiedName();
            
            int appID = getAppID() + 1;
            
            std::stringstream ss;
            ss << moduleName.toStdString();
            ss << ".createInstance(app" << appID;
            ss << ", app" << appID << "." << containerFullySpecifiedName;
            ss << ")\n";
            std::string err;
            if (!Natron::interpretPythonScript(ss.str(), &err, NULL)) {
                Natron::errorDialog(tr("Group plugin creation error").toStdString(), err);
                containerNode->destroyNode(false);
                return node;
            } else {
                node = containerNode;
            }
        } else {
            containerNode->loadKnobs(serialization);
            if (!serialization.isNull() && !serialization.getUserPages().empty()) {
                containerNode->getLiveInstance()->refreshKnobs();
            }
            node = containerNode;
        }
        
        if (!moduleName.isEmpty()) {
            setGroupLabelIDAndVersion(node,modulePath, moduleName);
        }
        
    } //FlagSetter fs(true,&_imp->_creatingGroup,&_imp->creatingGroupMutex);
    
    ///Now that the group is created and all nodes loaded, autoconnect the group like other nodes.
    onGroupCreationFinished(node);
    
    return node;
}


void
AppInstance::setGroupLabelIDAndVersion(const boost::shared_ptr<Natron::Node>& node,
                                       const QString& pythonModulePath,
                               const QString &pythonModule)
{
    std::string pluginID,pluginLabel,iconFilePath,pluginGrouping,description;
    unsigned int version;
    if (Natron::getGroupInfos(pythonModulePath.toStdString(),pythonModule.toStdString(), &pluginID, &pluginLabel, &iconFilePath, &pluginGrouping, &description, &version)) {
        node->setPluginIconFilePath(iconFilePath);
        node->setPluginDescription(description);
        node->setPluginIDAndVersionForGui(pluginLabel, pluginID, version);
        node->setPluginPythonModule(QString(pythonModulePath + pythonModule).toStdString());
    }
    
}



/**
 * @brief An inspector node is like a viewer node with hidden inputs that unfolds one after another.
 * This functions returns the number of inputs to use for inspectors or 0 for a regular node.
 **/
static int isEntitledForInspector(Natron::Plugin* plugin,OFX::Host::ImageEffect::Descriptor* ofxDesc)  {
    
    if (plugin->getPluginID() == PLUGINID_NATRON_VIEWER) {
        return 10;
    }
    
    if (!ofxDesc) {
        return 0;
    }
    
    const std::map<std::string,OFX::Host::ImageEffect::ClipDescriptor*>& clips = ofxDesc->getClips();
    int nInputs = 0;
    for (std::map<std::string,OFX::Host::ImageEffect::ClipDescriptor*>::const_iterator it = clips.begin(); it != clips.end(); ++it) {
        if (!it->second->isOutput()) {
            
            if (!it->second->isOptional()) {
                return 0;
            } else {
                ++nInputs;
            }
        }
    }
    if (nInputs > 4) {
        return nInputs;
    }
    return 0;
}

boost::shared_ptr<Natron::Node>
AppInstance::createNodeInternal(const QString & pluginID,
                                const std::string & multiInstanceParentName,
                                int majorVersion,
                                int minorVersion,
                                bool requestedByLoad,
                                const NodeSerialization & serialization,
                                bool dontLoadName,
                                bool autoConnect,
                                double xPosHint,
                                double yPosHint,
                                bool pushUndoRedoCommand,
                                bool addToProject,
                                bool userEdited,
                                const QString& fixedName,
                                const CreateNodeArgs::DefaultValuesList& paramValues,
                                const boost::shared_ptr<NodeCollection>& group)
{
    assert(group);
    
    boost::shared_ptr<Node> node;
    Natron::Plugin* plugin = 0;

    try {
        plugin = appPTR->getPluginBinary(pluginID,majorVersion,minorVersion,_imp->_projectCreatedWithLowerCaseIDs);
    } catch (const std::exception & e1) {
        
        ///Ok try with the old Ids we had in Natron prior to 1.0
        try {
            plugin = appPTR->getPluginBinaryFromOldID(pluginID, majorVersion, minorVersion);
        } catch (const std::exception& e2) {
            Natron::errorDialog(tr("Plugin error").toStdString(),
                                tr("Cannot load plugin executable").toStdString() + ": " + e2.what(), false );
            return node;
        }
        
    }
    
    
    if (!plugin) {
        return node;
    }
    

    const QString& pythonModule = plugin->getPythonModule();
    if (!pythonModule.isEmpty()) {
        return createNodeFromPythonModule(plugin, group, requestedByLoad, serialization);
    }

    std::string foundPluginID = plugin->getPluginID().toStdString();
    
    ContextEnum ctx;
    OFX::Host::ImageEffect::Descriptor* ofxDesc = plugin->getOfxDesc(&ctx);
    
    if (!ofxDesc) {
        OFX::Host::ImageEffect::ImageEffectPlugin* ofxPlugin = plugin->getOfxPlugin();
        if (ofxPlugin) {
            
            try {
                ofxDesc = Natron::OfxHost::getPluginContextAndDescribe(ofxPlugin,&ctx);
            } catch (const std::exception& e) {
                errorDialog(tr("Error while creating node").toStdString(), tr("Failed to create an instance of ").toStdString()
                            + pluginID.toStdString() + ": " + e.what(), false);
                return NodePtr();
            }
            assert(ofxDesc);
            plugin->setOfxDesc(ofxDesc, ctx);
        }
    }
    
    int nInputsForInspector = isEntitledForInspector(plugin,ofxDesc);
    
    if (!nInputsForInspector) {
        node.reset( new Node(this, addToProject ? group : boost::shared_ptr<NodeCollection>(), plugin) );
    } else {
        node.reset( new InspectorNode(this, addToProject ? group : boost::shared_ptr<NodeCollection>(), plugin,nInputsForInspector) );
    }
    
    {
        ///Furnace plug-ins don't handle using the thread pool
        boost::shared_ptr<Settings> settings = appPTR->getCurrentSettings();
        if (foundPluginID.find("uk.co.thefoundry.furnace") != std::string::npos &&
            (settings->useGlobalThreadPool() || settings->getNumberOfParallelRenders() != 1)) {
            Natron::StandardButtonEnum reply = Natron::questionDialog(tr("Warning").toStdString(),
                                                                  tr("The settings of the application are currently set to use "
                                                                     "the global thread-pool for rendering effects. The Foundry Furnace "
                                                                     "is known not to work well when this setting is checked. "
                                                                     "Would you like to turn it off ? ").toStdString(), false);
            if (reply == Natron::eStandardButtonYes) {
                settings->setUseGlobalThreadPool(false);
                settings->setNumberOfParallelRenders(1);
            }
        }
    }
    
    
    if (addToProject) {
        //Add the node to the project before loading it so it is present when the python script that registers a variable of the name
        //of the node works
        group->addNode(node);
    }
    assert(node);
    try {
        node->load(multiInstanceParentName, serialization,dontLoadName, userEdited,fixedName,paramValues);
    } catch (const std::exception & e) {
        group->removeNode(node);
        std::string title = std::string("Error while creating node");
        std::string message = title + " " + foundPluginID + ": " + e.what();
        qDebug() << message.c_str();
        errorDialog(title, message, false);

        return boost::shared_ptr<Natron::Node>();
    } catch (...) {
        group->removeNode(node);
        std::string title = std::string("Error while creating node");
        std::string message = title + " " + foundPluginID;
        qDebug() << message.c_str();
        errorDialog(title, message, false);

        return boost::shared_ptr<Natron::Node>();
    }

    boost::shared_ptr<Natron::Node> multiInstanceParent = node->getParentMultiInstance();

    // createNodeGui also sets the filename parameter for reader or writers
    createNodeGui(node,
                  multiInstanceParent,
                  requestedByLoad,
                  autoConnect,
                  xPosHint,
                  yPosHint,
                  pushUndoRedoCommand);
    
    boost::shared_ptr<NodeGroup> isGrp = boost::dynamic_pointer_cast<NodeGroup>(node->getLiveInstance()->shared_from_this());

    if (isGrp) {
        
        if (requestedByLoad) {
            if (!serialization.isNull() && !serialization.getPythonModule().empty()) {
                
                QString pythonModulePath(serialization.getPythonModule().c_str());
                QString moduleName;
                QString modulePath;
                int foundLastSlash = pythonModulePath.lastIndexOf('/');
                if (foundLastSlash != -1) {
                    modulePath = pythonModulePath.mid(0,foundLastSlash + 1);
                    moduleName = pythonModulePath.remove(0,foundLastSlash + 1);
                }
                setGroupLabelIDAndVersion(node, modulePath, moduleName);
            }
        } else if (!requestedByLoad && !_imp->_creatingGroup) {
            //if the node is a group and we're not loading the project, create one input and one output
            NodePtr input,output;
            
            {
                CreateNodeArgs args(PLUGINID_NATRON_OUTPUT,
                                    std::string(),
                                    -1,
                                    -1,
                                    false, //< don't autoconnect
                                    INT_MIN,
                                    INT_MIN,
                                    false, //<< don't push an undo command
                                    true,
                                    false,
                                    QString(),
                                    CreateNodeArgs::DefaultValuesList(),
                                    isGrp);
                output = createNode(args);
                output->setScriptName("Output");
                assert(output);
            }
            {
                CreateNodeArgs args(PLUGINID_NATRON_INPUT,
                                    std::string(),
                                    -1,
                                    -1,
                                    true, // autoconnect
                                    INT_MIN,
                                    INT_MIN,
                                    false, //<< don't push an undo command
                                    true,
                                    false,
                                    QString(),
                                    CreateNodeArgs::DefaultValuesList(),
                                    isGrp);
                input = createNode(args);
                assert(input);
            }
            
            ///Now that the group is created and all nodes loaded, autoconnect the group like other nodes.
            onGroupCreationFinished(node);
        }
    }
    
    return node;
} // createNodeInternal

boost::shared_ptr<Natron::Node>
AppInstance::createNode(const CreateNodeArgs & args)
{
    return createNodeInternal(args.pluginID,
                              args.multiInstanceParentName,
                              args.majorV, args.minorV,
                              false,
                              NodeSerialization( boost::shared_ptr<Natron::Node>() ),
                              !args.fixedName.isEmpty(),
                              args.autoConnect,
                              args.xPosHint,args.yPosHint,
                              args.pushUndoRedoCommand,
                              args.addToProject,
                              args.userEdited,
                              args.fixedName,
                              args.paramValues,
                              args.group);
}

boost::shared_ptr<Natron::Node>
AppInstance::loadNode(const LoadNodeArgs & args)
{
    return createNodeInternal(args.pluginID,
                              args.multiInstanceParentName,
                              args.majorV, args.minorV,
                              true,
                              *args.serialization,
                              args.dontLoadName,
                              false,
                              INT_MIN,INT_MIN,
                              false,
                              true,
                              true,
                              QString(),
                              CreateNodeArgs::DefaultValuesList(),
                              args.group);
}

int
AppInstance::getAppID() const
{
    return _imp->_appID;
}

boost::shared_ptr<Natron::Node>
AppInstance::getNodeByFullySpecifiedName(const std::string & name) const
{
    return _imp->_currentProject->getNodeByFullySpecifiedName(name);
}

boost::shared_ptr<Natron::Project>
AppInstance::getProject() const
{
    return _imp->_currentProject;
}

boost::shared_ptr<TimeLine>
AppInstance::getTimeLine() const
{
    return _imp->_currentProject->getTimeLine();
}

void
AppInstance::errorDialog(const std::string & title,
                         const std::string & message,
                         bool /*useHtml*/) const
{
    std::cout << "ERROR: " << title + ": " << message << std::endl;
}

void
AppInstance::errorDialog(const std::string & title,const std::string & message,bool* stopAsking,bool /*useHtml*/) const
{
    std::cout << "ERROR: " << title + ": " << message << std::endl;
    *stopAsking = false;
}

void
AppInstance::warningDialog(const std::string & title,
                           const std::string & message,
                           bool /*useHtml*/) const
{
    std::cout << "WARNING: " << title + ": " << message << std::endl;
}

void
AppInstance::warningDialog(const std::string & title,const std::string & message,bool* stopAsking,
                           bool /*useHtml*/) const
{
    std::cout << "WARNING: " << title + ": " << message << std::endl;
    *stopAsking = false;
}


void
AppInstance::informationDialog(const std::string & title,
                               const std::string & message,
                               bool /*useHtml*/) const
{
    std::cout << "INFO: " << title + ": " << message << std::endl;
}

void
AppInstance::informationDialog(const std::string & title,const std::string & message,bool* stopAsking,
                               bool /*useHtml*/) const
{
    std::cout << "INFO: " << title + ": " << message << std::endl;
    *stopAsking = false;
}


Natron::StandardButtonEnum
AppInstance::questionDialog(const std::string & title,
                            const std::string & message,
                            bool /*useHtml*/,
                            Natron::StandardButtons /*buttons*/,
                            Natron::StandardButtonEnum /*defaultButton*/) const
{
    std::cout << "QUESTION: " << title + ": " << message << std::endl;
    return Natron::eStandardButtonYes;
}



void
AppInstance::triggerAutoSave()
{
    _imp->_currentProject->triggerAutoSave();
}


void
AppInstance::startWritersRendering(const std::list<RenderRequest>& writers)
{
    std::list<RenderWork> renderers;

    if ( !writers.empty() ) {
        for (std::list<RenderRequest>::const_iterator it = writers.begin(); it != writers.end(); ++it) {
            
            std::string writerName =  it->writerName.toStdString();
            
            NodePtr node = getNodeByFullySpecifiedName(writerName);
           
            if (!node) {
                std::string exc(writerName);
                exc.append(tr(" does not belong to the project file. Please enter a valid writer name.").toStdString());
                throw std::invalid_argument(exc);
            } else {
                if ( !node->isOutputNode() ) {
                    std::string exc(writerName);
                    exc.append(" is not an output node! It cannot render anything.");
                    throw std::invalid_argument(exc);
                }
                ViewerInstance* isViewer = dynamic_cast<ViewerInstance*>(node->getLiveInstance());
                if (isViewer) {
                    throw std::invalid_argument("Internal issue with the project loader...viewers should have been evicted from the project.");
                }
                
                RenderWork w;
                w.writer = dynamic_cast<OutputEffectInstance*>( node->getLiveInstance() );
                assert(w.writer);
                w.firstFrame = it->firstFrame;
                w.lastFrame = it->lastFrame;
                renderers.push_back(w);
            }
        }
    } else {
        //start rendering for all writers found in the project
        std::list<Natron::OutputEffectInstance*> writers;
        getProject()->getWriters(&writers);
        
        for (std::list<Natron::OutputEffectInstance*>::const_iterator it2 = writers.begin(); it2 != writers.end(); ++it2) {
            RenderWork w;
            w.writer = *it2;
            assert(w.writer);
            if (w.writer) {
                w.writer->getFrameRange_public(w.writer->getHash(), &w.firstFrame, &w.lastFrame);
            }
            renderers.push_back(w);
        }
    }
    
    startWritersRendering(renderers);
}

void
AppInstance::startWritersRendering(const std::list<RenderWork>& writers)
{
    
    if (writers.empty()) {
        return;
    }
    
    if ( appPTR->isBackground() ) {
        
        //blocking call, we don't want this function to return pre-maturely, in which case it would kill the app
        QtConcurrent::blockingMap( writers,boost::bind(&AppInstance::startRenderingFullSequence,this,_1,false,QString()) );
    } else {
        
        //Take a snapshot of the graph at this time, this will be the version loaded by the process
        bool renderInSeparateProcess = appPTR->getCurrentSettings()->isRenderInSeparatedProcessEnabled();
        QString savePath = getProject()->saveProject("","RENDER_SAVE.ntp",true);

        for (std::list<RenderWork>::const_iterator it = writers.begin();it!=writers.end();++it) {
            ///Use the frame range defined by the writer GUI because we're in an interactive session
            startRenderingFullSequence(*it,renderInSeparateProcess,savePath);
        }
    }
}

void
AppInstance::startRenderingFullSequence(const RenderWork& writerWork,bool /*renderInSeparateProcess*/,const QString& /*savePath*/)
{
    BlockingBackgroundRender backgroundRender(writerWork.writer);
    int first,last;
    if (writerWork.firstFrame == INT_MIN || writerWork.lastFrame == INT_MAX) {
        writerWork.writer->getFrameRange_public(writerWork.writer->getHash(), &first, &last);
        if (first == INT_MIN || last == INT_MAX) {
            getFrameRange(&first, &last);
        }
    } else {
        first = writerWork.firstFrame;
        last = writerWork.lastFrame;
    }
    
    backgroundRender.blockingRender(first,last); //< doesn't return before rendering is finished
}

void
AppInstance::getFrameRange(int* first,int* last) const
{
    return _imp->_currentProject->getFrameRange(first, last);
}

void
AppInstance::clearOpenFXPluginsCaches()
{
    NodeList activeNodes;
    _imp->_currentProject->getActiveNodes(&activeNodes);

    for (NodeList::iterator it = activeNodes.begin(); it != activeNodes.end(); ++it) {
        (*it)->purgeAllInstancesCaches();
    }
}

void
AppInstance::clearAllLastRenderedImages()
{
    NodeList activeNodes;
    _imp->_currentProject->getActiveNodes(&activeNodes);
    
    for (NodeList::iterator it = activeNodes.begin(); it != activeNodes.end(); ++it) {
        (*it)->clearLastRenderedImage();
    }
}



void
AppInstance::quit()
{
    appPTR->quit(this);
}

Natron::ViewerColorSpaceEnum
AppInstance::getDefaultColorSpaceForBitDepth(Natron::ImageBitDepthEnum bitdepth) const
{
    return _imp->_currentProject->getDefaultColorSpaceForBitDepth(bitdepth);
}

int
AppInstance::getMainView() const
{
    return _imp->_currentProject->getProjectMainView();
}

void
AppInstance::onOCIOConfigPathChanged(const std::string& path)
{
    _imp->_currentProject->onOCIOConfigPathChanged(path,false);
}

void
AppInstance::declareCurrentAppVariable_Python()
{
    /// define the app variable
    std::stringstream ss;
    ss << "app" << _imp->_appID + 1 << " = natron.getInstance(" << _imp->_appID << ") \n";
    const std::vector<boost::shared_ptr<KnobI> >& knobs = _imp->_currentProject->getKnobs();
    for (std::vector<boost::shared_ptr<KnobI> >::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        ss << "app" << _imp->_appID + 1 << "." << (*it)->getName() << " = app" << _imp->_appID + 1 << ".getProjectParam('" <<
        (*it)->getName() << "')\n";
    }
    std::string script = ss.str();
    std::string err;
    
    bool ok = Natron::interpretPythonScript(script, &err, 0);
    assert(ok);
    (void)ok;

    if (appPTR->isBackground()) {
        std::string err;
        ok = Natron::interpretPythonScript("app = app1\n", &err, 0);
        assert(ok);
    }
}


double
AppInstance::getProjectFrameRate() const
{
    return _imp->_currentProject->getProjectFrameRate();
}

void
AppInstance::setProjectWasCreatedWithLowerCaseIDs(bool b)
{
    _imp->_projectCreatedWithLowerCaseIDs = b;
}

bool
AppInstance::wasProjectCreatedWithLowerCaseIDs() const
{
    return _imp->_projectCreatedWithLowerCaseIDs;
}

bool
AppInstance::isCreatingPythonGroup() const
{
    QMutexLocker k(&_imp->creatingGroupMutex);
    return _imp->_creatingGroup;
}

void
AppInstance::appendToScriptEditor(const std::string& str)
{
    std::cout << str <<  std::endl;
}

void
AppInstance::printAutoDeclaredVariable(const std::string& /*str*/)
{
    
}

void
AppInstance::execOnProjectCreatedCallback()
{
    std::string cb = appPTR->getCurrentSettings()->getOnProjectCreatedCB();
    if (cb.empty()) {
        return;
    }
    
    
    std::vector<std::string> args;
    std::string error;
    Natron::getFunctionArguments(cb, &error, &args);
    if (!error.empty()) {
        appendToScriptEditor("Failed to run onProjectCreated callback: " + error);
        return;
    }
    
    std::string signatureError;
    signatureError.append("The on project created callback supports the following signature(s):\n");
    signatureError.append("- callback(app)");
    if (args.size() != 1) {
        appendToScriptEditor("Failed to run onProjectCreated callback: " + signatureError);
        return;
    }
    if (args[0] != "app") {
        appendToScriptEditor("Failed to run onProjectCreated callback: " + signatureError);
        return;
    }
    std::string appID = getAppIDString();
    std::string script = "app = " + appID + "\n" + cb + "(" + appID + ")\n";
    std::string err;
    std::string output;
    if (!Natron::interpretPythonScript(script, &err, &output)) {
        appendToScriptEditor("Failed to run onProjectCreated callback: " + err);
    } else {
        if (!output.empty()) {
            appendToScriptEditor(output);
        }
    }
}

std::string
AppInstance::getAppIDString() const
{
    if (appPTR->isBackground()) {
        return std::string("app");
    } else {
        QString appID =  QString("app%1").arg(getAppID() + 1);
        return appID.toStdString();
    }
}

void
AppInstance::onGroupCreationFinished(const boost::shared_ptr<Natron::Node>& /*node*/)
{
//    assert(node);
//    if (!_imp->_currentProject->isLoadingProject()) {
//        NodeGroup* isGrp = dynamic_cast<NodeGroup*>(node->getLiveInstance());
//        assert(isGrp);
//        if (!isGrp) {
//            return;
//        }
//        isGrp->forceGetClipPreferencesOnAllTrees();
//    }
}
