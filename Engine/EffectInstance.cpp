//  Natron
//
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

#include "EffectInstance.h"

#include <map>
#include <sstream>
#include <QtConcurrentMap>
#include <QReadWriteLock>
#include <QCoreApplication>
#include <QtConcurrentRun>
#if !defined(SBK_RUN) && !defined(Q_MOC_RUN)
#include <boost/bind.hpp>
#endif
#include <SequenceParsing.h>

#include "Global/MemoryInfo.h"
#include "Engine/AppManager.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/Node.h"
#include "Engine/ViewerInstance.h"
#include "Engine/Log.h"
#include "Engine/Image.h"
#include "Engine/ImageParams.h"
#include "Engine/KnobFile.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/OfxImageEffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/PluginMemory.h"
#include "Engine/Project.h"
#include "Engine/BlockingBackgroundRender.h"
#include "Engine/AppInstance.h"
#include "Engine/ThreadStorage.h"
#include "Engine/Settings.h"
#include "Engine/RotoContext.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/Transform.h"
#include "Engine/DiskCacheNode.h"

//#define NATRON_ALWAYS_ALLOCATE_FULL_IMAGE_BOUNDS

using namespace Natron;


class File_Knob;
class OutputFile_Knob;



namespace  {
    struct ActionKey {
        double time;
        int view;
        unsigned int mipMapLevel;
    };
    
    struct IdentityResults {
        int inputIdentityNb;
        double inputIdentityTime;
    };
    
    struct CompareActionsCacheKeys {
        bool operator() (const ActionKey& lhs,const ActionKey& rhs) const {
            if (lhs.time < rhs.time) {
                return true;
            } else if (lhs.time == rhs.time) {
                if (lhs.mipMapLevel < rhs.mipMapLevel) {
                    return true;
                } else if (lhs.mipMapLevel == rhs.mipMapLevel) {
                    if (lhs.view < rhs.view) {
                        return true;
                    } else {
                        return false;
                    }
                } else {
                    return false;
                }
            } else {
                return false;
            }
            
        }
    };
    
    typedef std::map<ActionKey,IdentityResults,CompareActionsCacheKeys> IdentityCacheMap;
    typedef std::map<ActionKey,RectD,CompareActionsCacheKeys> RoDCacheMap;
    
    /**
     * @brief This class stores all results of the following actions:
     - getRegionOfDefinition (invalidated on hash change, mapped across time + scale)
     - getTimeDomain (invalidated on hash change, only 1 value possible
     - isIdentity (invalidated on hash change,mapped across time + scale)
     * The reason we store them is that the OFX Clip API can potentially call these actions recursively
     * but this is forbidden by the spec:
     * http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#id475585
     **/
    class ActionsCache {
        
        mutable QMutex _cacheMutex; //< protects everything in the cache
        
        U64 _cacheHash; //< the effect hash at which the actions were computed
        
        OfxRangeD _timeDomain;
        bool _timeDomainSet;
        
        IdentityCacheMap _identityCache;
        RoDCacheMap _rodCache;
        
    public:
        
        ActionsCache()
        : _cacheMutex()
        , _cacheHash(0)
        , _timeDomain()
        , _timeDomainSet(false)
        , _identityCache()
        , _rodCache()
        {
            
        }
        
        /**
         * @brief Get the hash at which the actions are stored in the cache currently
         **/
        bool getCacheHash() const {
            QMutexLocker l(&_cacheMutex);
            return _cacheHash;
        }
        
        void invalidateAll(U64 newHash) {
            QMutexLocker l(&_cacheMutex);
            _cacheHash = newHash;
            _rodCache.clear();
            _identityCache.clear();
            _timeDomainSet = false;
        }
        
        
        bool getIdentityResult(U64 hash,double time, int view, unsigned int mipMapLevel,int* inputNbIdentity,double* identityTime) {
            QMutexLocker l(&_cacheMutex);
            if (hash != _cacheHash)
                return false;
            
            ActionKey key;
            key.time = time;
            key.view = view;
            key.mipMapLevel = mipMapLevel;
            
            IdentityCacheMap::const_iterator found = _identityCache.find(key);
            if ( found != _identityCache.end() ) {
                *inputNbIdentity = found->second.inputIdentityNb;
                *identityTime = found->second.inputIdentityTime;
                return true;
            }
            return false;
        }
        
        void setIdentityResult(double time, int view, unsigned int mipMapLevel,int inputNbIdentity,double identityTime)
        {
            QMutexLocker l(&_cacheMutex);
           
            
            ActionKey key;
            key.time = time;
            key.view = view;
            key.mipMapLevel = mipMapLevel;
            
            IdentityCacheMap::iterator found = _identityCache.find(key);
            if ( found != _identityCache.end() ) {
                found->second.inputIdentityNb = inputNbIdentity;
                found->second.inputIdentityTime = identityTime;
            } else {
                IdentityResults v;
                v.inputIdentityNb = inputNbIdentity;
                v.inputIdentityTime = identityTime;
                _identityCache.insert(std::make_pair(key, v));
            }
            
        }
        
        bool getRoDResult(U64 hash,double time, int view,unsigned int mipMapLevel,RectD* rod) {
            QMutexLocker l(&_cacheMutex);
            if (hash != _cacheHash)
                return false;
            
            ActionKey key;
            key.time = time;
            key.view = view;
            key.mipMapLevel = mipMapLevel;
            
            RoDCacheMap::const_iterator found = _rodCache.find(key);
            if ( found != _rodCache.end() ) {
                *rod = found->second;
                return true;
            }
            return false;
        }
        
        void setRoDResult(double time, int view, unsigned int mipMapLevel,const RectD& rod)
        {
            QMutexLocker l(&_cacheMutex);
            
            
            ActionKey key;
            key.time = time;
            key.view = view;
            key.mipMapLevel = mipMapLevel;
            
            RoDCacheMap::iterator found = _rodCache.find(key);
            if ( found != _rodCache.end() ) {
                ///Already set, this is a bug
                return;
            } else {
                _rodCache.insert(std::make_pair(key, rod));
            }
            
        }
        
        bool getTimeDomainResult(U64 hash,double *first,double* last) {
            QMutexLocker l(&_cacheMutex);
            if (hash != _cacheHash || !_timeDomainSet)
                return false;
            
            *first = _timeDomain.min;
            *last = _timeDomain.max;
            return true;
        }
        
        void setTimeDomainResult(double first,double last)
        {
            QMutexLocker l(&_cacheMutex);
            _timeDomainSet = true;
            _timeDomain.min = first;
            _timeDomain.max = last;
        }
        
    };

}

/**
 * @brief These args are local to a renderRoI call and used to retrieve this info 
 * in a thread-safe and thread-local manner in getImage
 **/
struct EffectInstance::RenderArgs
{
    RectD _rod; //!< the effect's RoD in CANONICAL coordinates
    RoIMap _regionOfInterestResults; //< the input RoI's in CANONICAL coordinates
    RectI _renderWindowPixel; //< the current renderWindow in PIXEL coordinates
    SequenceTime _time; //< the time to render
    int _view; //< the view to render
    bool _validArgs; //< are the args valid ?
    int _channelForAlpha;
    bool _isIdentity;
    SequenceTime _identityTime;
    int _identityInputNb;
    std::map<Natron::ImageComponents,PlaneToRender> _outputPlanes;
    
    int _firstFrame,_lastFrame;
    
    RenderArgs()
        : _rod()
          , _regionOfInterestResults()
          , _renderWindowPixel()
          , _time(0)
          , _view(0)
          , _validArgs(false)
          , _channelForAlpha(3)
          , _isIdentity(false)
          , _identityTime(0)
          , _identityInputNb(-1)
          , _outputPlanes()
          , _firstFrame(0)
          , _lastFrame(0)
    {
    }
    
    RenderArgs(const RenderArgs& o)
    : _rod(o._rod)
    , _regionOfInterestResults(o._regionOfInterestResults)
    , _renderWindowPixel(o._renderWindowPixel)
    , _time(o._time)
    , _view(o._view)
    , _validArgs(o._validArgs)
    , _channelForAlpha(o._channelForAlpha)
    , _isIdentity(o._isIdentity)
    , _identityTime(o._identityTime)
    , _identityInputNb(o._identityInputNb)
    , _outputPlanes(o._outputPlanes)
    , _firstFrame(o._firstFrame)
    , _lastFrame(o._lastFrame)
    {
    }
};


struct EffectInstance::Implementation
{
    Implementation(EffectInstance* publicInterface)
    : _publicInterface(publicInterface)
    , renderAbortedMutex()
    , renderAborted(false)
    , renderArgs()
    , frameRenderArgs()
    , beginEndRenderCount()
    , inputImages()
    , lastRenderArgsMutex()
    , lastRenderHash(0)
    , lastPlanesRendered()
    , duringInteractActionMutex()
    , duringInteractAction(false)
    , pluginMemoryChunksMutex()
    , pluginMemoryChunks()
    , supportsRenderScale(eSupportsMaybe)
    , actionsCache()
#if NATRON_ENABLE_TRIMAP
    , imagesBeingRenderedMutex()
    , imagesBeingRendered()
#endif
    {
    }

    EffectInstance* _publicInterface;
    
    mutable QReadWriteLock renderAbortedMutex;
    bool renderAborted; //< was rendering aborted ?

    ///Thread-local storage living through the render_public action and used by getImage to retrieve all parameters
    ThreadStorage<RenderArgs> renderArgs;
    
    ///Thread-local storage living through the whole rendering of a frame
    ThreadStorage<ParallelRenderArgs> frameRenderArgs;
    
    ///Keep track of begin/end sequence render calls to make sure they are called in the right order even when
    ///recursive renders are called
    ThreadStorage<int> beginEndRenderCount;

    ///Whenever a render thread is running, it stores here a temp copy used in getImage
    ///to make sure these images aren't cleared from the cache.
    ThreadStorage< std::list< boost::shared_ptr<Natron::Image> > > inputImages;
    

    QMutex lastRenderArgsMutex; //< protects lastImage & lastRenderHash
    U64 lastRenderHash;  //< the last hash given to render
    ImageList lastPlanesRendered; //< the last image planes rendered
    
    mutable QReadWriteLock duringInteractActionMutex; //< protects duringInteractAction
    bool duringInteractAction; //< true when we're running inside an interact action
    
    ///Current chuncks of memory held by the plug-in
    mutable QMutex pluginMemoryChunksMutex;
    std::list<PluginMemory*> pluginMemoryChunks;
    
    ///Does this plug-in supports render scale ?
    QMutex supportsRenderScaleMutex;
    SupportsEnum supportsRenderScale;

    /// Mt-Safe actions cache
    ActionsCache actionsCache;
    
#if NATRON_ENABLE_TRIMAP
    ///Store all images being rendered to avoid 2 threads rendering the same portion of an image
    struct ImageBeingRendered
    {
        QWaitCondition cond;
        QMutex lock;
        int refCount;
        bool renderFailed;
        
        ImageBeingRendered() : cond(), lock(), refCount(0), renderFailed(false) {}
    };
    QMutex imagesBeingRenderedMutex;
    typedef boost::shared_ptr<ImageBeingRendered> IBRPtr;
    typedef std::map<ImagePtr,IBRPtr > IBRMap;
    IBRMap imagesBeingRendered;
#endif
    
    void runChangedParamCallback(KnobI* k,bool userEdited,const std::string& callback);
    
    
    void setDuringInteractAction(bool b)
    {
        QWriteLocker l(&duringInteractActionMutex);

        duringInteractAction = b;
    }

#if NATRON_ENABLE_TRIMAP
    void markImageAsBeingRendered(const boost::shared_ptr<Natron::Image>& img)
    {
        if (!img->usesBitMap()) {
            return;
        }
        QMutexLocker k(&imagesBeingRenderedMutex);
        IBRMap::iterator found = imagesBeingRendered.find(img);
        if (found != imagesBeingRendered.end()) {
            ++(found->second->refCount);
        } else {
            IBRPtr ibr(new Implementation::ImageBeingRendered);
            ++ibr->refCount;
            imagesBeingRendered.insert(std::make_pair(img,ibr));
        }
    }
    
    void waitForImageBeingRenderedElsewhereAndUnmark(const RectI& roi,const boost::shared_ptr<Natron::Image>& img)
    {
        if (!img->usesBitMap()) {
            return;
        }
        IBRPtr ibr;
        {
            QMutexLocker k(&imagesBeingRenderedMutex);
            IBRMap::iterator found = imagesBeingRendered.find(img);
            assert(found != imagesBeingRendered.end());
            ibr = found->second;
        }
        
        std::list<RectI> restToRender;
        bool isBeingRenderedElseWhere = false;
        img->getRestToRender_trimap(roi,restToRender, &isBeingRenderedElseWhere);
        
        bool ab = _publicInterface->aborted();
        {
            QMutexLocker kk(&ibr->lock);
            while (!ab && isBeingRenderedElseWhere && !ibr->renderFailed) {
                ibr->cond.wait(&ibr->lock);
                isBeingRenderedElseWhere = false;
                img->getRestToRender_trimap(roi, restToRender, &isBeingRenderedElseWhere);
                ab = _publicInterface->aborted();
            }
        }
        
        ///Everything should be rendered now.
        assert(ab || restToRender.empty());

        {
            QMutexLocker k(&imagesBeingRenderedMutex);
            IBRMap::iterator found = imagesBeingRendered.find(img);
            assert(found != imagesBeingRendered.end());
            
            QMutexLocker kk(&ibr->lock);
            --ibr->refCount;
            found->second->cond.wakeAll();
            if (found != imagesBeingRendered.end() && !ibr->refCount) {
                imagesBeingRendered.erase(found);
            }
        }

        
        
    
    }
    
    void unmarkImageAsBeingRendered(const boost::shared_ptr<Natron::Image>& img,bool renderFailed)
    {
        if (!img->usesBitMap()) {
            return;
        }
        QMutexLocker k(&imagesBeingRenderedMutex);
        IBRMap::iterator found = imagesBeingRendered.find(img);
        assert(found != imagesBeingRendered.end());
        
        QMutexLocker kk(&found->second->lock);
        if (renderFailed) {
            found->second->renderFailed = true;
        }
        found->second->cond.wakeAll();
        --found->second->refCount;
        if (!found->second->refCount) {
            kk.unlock(); // < unlock before erase which is going to delete the lock
            imagesBeingRendered.erase(found);
        }
        
    }
#endif
    /**
     * @brief This function sets on the thread storage given in parameter all the arguments which
     * are used to render an image.
     * This is used exclusively on the render thread in the renderRoI function or renderRoIInternal function.
     * The reason we use thread-storage is because the OpenFX API doesn't give all the parameters to the 
     * ImageEffect suite functions except the desired time. That is the Host has to maintain an internal state to "guess" what are the
     * expected parameters in order to respond correctly to the function call. This state is maintained throughout the render thread work
     * for all these actions:
     * 
        - getRegionsOfInterest
        - getFrameRange
        - render
        - beginRender
        - endRender
        - isIdentity
     *
     * The object that will need to know these datas is OfxClipInstance, more precisely in the following functions:
        - OfxClipInstance::getRegionOfDefinition
        - OfxClipInstance::getImage
     * 
     * We don't provide these datas for the getRegionOfDefinition with these render args because this action can be called way
     * prior we have all the other parameters. getRegionOfDefinition only needs the current render view and mipMapLevel if it is
     * called on a render thread or during an analysis. We provide it by setting those 2 parameters directly on a thread-storage 
     * object local to the clip.
     *
     * For getImage, all the ScopedRenderArgs are active (except for analysis). The view and mipMapLevel parameters will be retrieved
     * on the clip that needs the image. All the other parameters will be retrieved in EffectInstance::getImage on the ScopedRenderArgs.
     *
     * During an analysis effect we don't set any ScopedRenderArgs and call some actions recursively if needed.
     * WARNING: analysis effect's are set the current view and mipmapLevel to 0 in the OfxEffectInstance::knobChanged function
     * If we were to have analysis that perform on different views we would have to change that.
     **/
    class ScopedRenderArgs
    {
        RenderArgs* localData;
        ThreadStorage<RenderArgs>* _dst;
        
    public:
        
        ScopedRenderArgs(ThreadStorage<RenderArgs>* dst,
                         const RoIMap & roiMap,
                         const RectD & rod,
                         const RectI& renderWindow,
                         SequenceTime time,
                         int view,
                         int channelForAlpha,
                         bool isIdentity,
                         SequenceTime identityTime,
                         int inputNbIdentity,
                         const std::map<Natron::ImageComponents,PlaneToRender>& outputPlanes,
                         int firstFrame,
                         int lastFrame)
            : localData(&dst->localData())
            , _dst(dst)
        {
            assert(_dst);

            localData->_rod = rod;
            localData->_renderWindowPixel = renderWindow;
            localData->_time = time;
            localData->_view = view;
            localData->_channelForAlpha = channelForAlpha;
            localData->_isIdentity = isIdentity;
            localData->_identityTime = identityTime;
            localData->_identityInputNb = inputNbIdentity;
            localData->_outputPlanes = outputPlanes;
            localData->_regionOfInterestResults = roiMap;
            localData->_firstFrame = firstFrame;
            localData->_lastFrame = lastFrame;
            localData->_validArgs = true;

        }
        
        ScopedRenderArgs(ThreadStorage<RenderArgs>* dst)
        : localData(&dst->localData())
        , _dst(dst)
        {
            assert(_dst);
        }

        

        ScopedRenderArgs(ThreadStorage<RenderArgs>* dst,
                         const RenderArgs & a)
            : localData(&dst->localData())
              , _dst(dst)
        {
            *localData = a;
            localData->_validArgs = true;
        }

        ~ScopedRenderArgs()
        {
            assert( _dst->hasLocalData() );
            localData->_outputPlanes.clear();
            localData->_validArgs = false;
        }

        RenderArgs& getLocalData() {
            return *localData;
        }
        
        ///Setup the first pass on thread-local storage.
        ///RoIMap and frame range are separated because those actions might need
        ///the thread-storage set up in the first pass to work
        void setArgs_firstPass(const RectD & rod,
                               const RectI& renderWindow,
                               SequenceTime time,
                               int view,
                               int channelForAlpha,
                               bool isIdentity,
                               SequenceTime identityTime,
                               int inputNbIdentity)
        {
            localData->_rod = rod;
            localData->_renderWindowPixel = renderWindow;
            localData->_time = time;
            localData->_view = view;
            localData->_channelForAlpha = channelForAlpha;
            localData->_isIdentity = isIdentity;
            localData->_identityTime = identityTime;
            localData->_identityInputNb = inputNbIdentity;
            localData->_validArgs = true;
        }
        
        void setArgs_secondPass(const RoIMap & roiMap,
                                int firstFrame,
                                int lastFrame) {
            localData->_regionOfInterestResults = roiMap;
            localData->_firstFrame = firstFrame;
            localData->_lastFrame = lastFrame;
            localData->_validArgs = true;
        }
        

    };
    
    void addInputImageTempPointer(const boost::shared_ptr<Natron::Image> & img)
    {
        inputImages.localData().push_back(img);
    }

    void clearInputImagePointers()
    {
        if (inputImages.hasLocalData()) {
            inputImages.localData().clear();
        }
    }
};

class InputImagesHolder_RAII
{
    ThreadStorage< std::list< boost::shared_ptr<Natron::Image> > > *storage;
    
public:
    
    InputImagesHolder_RAII(const std::list<boost::shared_ptr<Natron::Image> >& imgs,ThreadStorage< std::list< boost::shared_ptr<Natron::Image> > >* storage)
    : storage(storage)
    {
        if (!imgs.empty()) {
            std::list<boost::shared_ptr<Natron::Image> >& data = storage->localData();
            data.insert(data.begin(), imgs.begin(),imgs.end());
        } else {
            this->storage = 0;
        }
        
    }
    
    ~InputImagesHolder_RAII()
    {
        if (storage) {
            assert(storage->hasLocalData());
            storage->localData().clear();
        }
    }
};

void
EffectInstance::addThreadLocalInputImageTempPointer(const boost::shared_ptr<Natron::Image> & img)
{
    _imp->addInputImageTempPointer(img);
}

EffectInstance::EffectInstance(boost::shared_ptr<Node> node)
    : NamedKnobHolder(node ? node->getApp() : NULL)
      , _node(node)
      , _imp(new Implementation(this))
{
}

EffectInstance::~EffectInstance()
{
    clearPluginMemoryChunks();
}


void
EffectInstance::lock(const boost::shared_ptr<Natron::Image>& entry)
{
    boost::shared_ptr<Node> n = _node.lock();
    n->lock(entry);
}


bool
EffectInstance::tryLock(const boost::shared_ptr<Natron::Image>& entry)
{
    boost::shared_ptr<Node> n = _node.lock();
    return n->tryLock(entry);
}

void
EffectInstance::unlock(const boost::shared_ptr<Natron::Image>& entry)
{
    boost::shared_ptr<Node> n = _node.lock();
    n->unlock(entry);
}

void
EffectInstance::clearPluginMemoryChunks()
{
    int toRemove;
    {
        QMutexLocker l(&_imp->pluginMemoryChunksMutex);
        toRemove = (int)_imp->pluginMemoryChunks.size();
    }

    while (toRemove > 0) {
        PluginMemory* mem;
        {
            QMutexLocker l(&_imp->pluginMemoryChunksMutex);
            mem = ( *_imp->pluginMemoryChunks.begin() );
        }
        delete mem;
        --toRemove;
    }
}

void
EffectInstance::setParallelRenderArgsTLS(int time,
                                         int view,
                                         bool isRenderUserInteraction,
                                         bool isSequential,
                                         bool canAbort,
                                         U64 nodeHash,
                                         U64 rotoAge,
                                         U64 renderAge,
                                         ViewerInstance* viewer,
                                         int textureIndex,
                                         const TimeLine* timeline)
{
    ParallelRenderArgs& args = _imp->frameRenderArgs.localData();
    args.time = time;
    args.timeline = timeline;
    args.view = view;
    args.isRenderResponseToUserInteraction = isRenderUserInteraction;
    args.isSequentialRender = isSequential;
    
    args.nodeHash = nodeHash;
    args.rotoAge = rotoAge;
    args.canAbort = canAbort;
    args.renderAge = renderAge;
    args.renderRequester = viewer;
    args.textureIndex = textureIndex;
    
    ++args.validArgs;
    
}

void
EffectInstance::setParallelRenderArgsTLS(const ParallelRenderArgs& args)
{
    assert(args.validArgs);
    ParallelRenderArgs& tls = _imp->frameRenderArgs.localData();
    int curValid = tls.validArgs;
    tls = args;
    tls.validArgs = curValid + 1;
}


void
EffectInstance::invalidateParallelRenderArgsTLS()
{
    if (_imp->frameRenderArgs.hasLocalData()) {
        ParallelRenderArgs& args = _imp->frameRenderArgs.localData();
        --args.validArgs;
        if (args.validArgs < 0) {
            args.validArgs = 0;
        }
    } else {
        qDebug() << "Frame render args thread storage not set, this is probably because the graph changed while rendering.";
    }
}

ParallelRenderArgs
EffectInstance::getParallelRenderArgsTLS() const
{
    if (_imp->frameRenderArgs.hasLocalData()) {
        return _imp->frameRenderArgs.localData();
    } else {
        qDebug() << "Frame render args thread storage not set, this is probably because the graph changed while rendering.";
        return ParallelRenderArgs();
    }
}

U64
EffectInstance::getHash() const
{
    boost::shared_ptr<Node> n = _node.lock();
    return n->getHashValue();
}

U64
EffectInstance::getRenderHash() const
{
    
    if ( !_imp->frameRenderArgs.hasLocalData() ) {
        return getHash();
    } else {
        ParallelRenderArgs& args = _imp->frameRenderArgs.localData();
        if (!args.validArgs) {
            return getHash();
        } else {
            return args.nodeHash;
        }
    }
}

bool
EffectInstance::isAbortedFromPlayback() const
{
    
    ///This flag is set in OutputSchedulerThread::abortRendering
    ///This will be used when playback or rendering on disk
    QReadLocker l(&_imp->renderAbortedMutex);
    return _imp->renderAborted;
    
}

bool
EffectInstance::aborted() const
{
   
     if ( !_imp->frameRenderArgs.hasLocalData() ) {
        
        ///No local data, we're either not rendering or calling this from a thread not controlled by Natron
        return isAbortedFromPlayback();
        
    } else {
        
        ParallelRenderArgs& args = _imp->frameRenderArgs.localData();
        if (!args.validArgs) {
            ///No valid args, probably not rendering
            return false;
        } else {
            if (args.isRenderResponseToUserInteraction) {
                
                if (args.canAbort) {
                    if (args.renderRequester && !args.renderRequester->isRenderAbortable(args.textureIndex, args.renderAge)) {
                        return false;
                    }
                    
                    ///Rendering issued by RenderEngine::renderCurrentFrame, if time or hash changed, abort
                    bool ret = (args.nodeHash != getHash() ||
                                args.time != args.timeline->currentFrame() ||
                                !getNode()->isActivated());
                    return ret;
                } else {
                    bool ret = !getNode()->isActivated();
                    return ret;
                }
                
            } else {
                ///Rendering is playback or render on disk, we rely on the _imp->renderAborted flag for this.

                return isAbortedFromPlayback();
          
            }
        }

    }
}

bool
EffectInstance::shouldCacheOutput() const
{
    boost::shared_ptr<Node> n = _node.lock();
    return n->shouldCacheOutput();
}

void
EffectInstance::setAborted(bool b)
{
    {
        QWriteLocker l(&_imp->renderAbortedMutex);
        _imp->renderAborted = b;
    }
}

U64
EffectInstance::getKnobsAge() const
{
    return getNode()->getKnobsAge();
}

void
EffectInstance::setKnobsAge(U64 age)
{
    getNode()->setKnobsAge(age);
}

const std::string &
EffectInstance::getScriptName() const
{
    return getNode()->getScriptName();
}

std::string
EffectInstance::getScriptName_mt_safe() const
{
    return getNode()->getScriptName_mt_safe();
}

void
EffectInstance::getRenderFormat(Format *f) const
{
    assert(f);
    getApp()->getProject()->getProjectDefaultFormat(f);
}

int
EffectInstance::getRenderViewsCount() const
{
    return getApp()->getProject()->getProjectViewsCount();
}

bool
EffectInstance::hasOutputConnected() const
{
    return getNode()->hasOutputConnected();
}

EffectInstance*
EffectInstance::getInput(int n) const
{
    boost::shared_ptr<Natron::Node> inputNode = getNode()->getInput(n);

    if (inputNode) {
        return inputNode->getLiveInstance();
    }

    return NULL;
}

std::string
EffectInstance::getInputLabel(int inputNb) const
{
    std::string out;

    out.append( 1,(char)(inputNb + 65) );

    return out;
}

bool
EffectInstance::retrieveGetImageDataUponFailure(const int time,
                                                const int view,
                                                const RenderScale& scale,
                                                const RectD* optionalBoundsParam,
                                                U64* nodeHash_p,
                                                U64* rotoAge_p,
                                                bool* isIdentity_p,
                                                int* identityTime,
                                                int* identityInputNb_p,
                                                RectD* rod_p,
                                                RoIMap* inputRois_p, //!< output, only set if optionalBoundsParam != NULL
                                                RectD* optionalBounds_p) //!< output, only set if optionalBoundsParam != NULL
{
    /////Update 09/02/14
    /// We now AUTHORIZE GetRegionOfDefinition and isIdentity and getRegionsOfInterest to be called recursively.
    /// It didn't make much sense to forbid them from being recursive.
    
//#ifdef DEBUG
//    if (QThread::currentThread() != qApp->thread()) {
//        ///This is a bad plug-in
//        qDebug() << getNode()->getScriptName_mt_safe().c_str() << " is trying to call clipGetImage during an unauthorized time. "
//        "Developers of that plug-in should fix it. \n Reminder from the OpenFX spec: \n "
//        "Images may be fetched from an attached clip in the following situations... \n"
//        "- in the kOfxImageEffectActionRender action\n"
//        "- in the kOfxActionInstanceChanged and kOfxActionEndInstanceChanged actions with a kOfxPropChangeReason or kOfxChangeUserEdited";
//    }
//#endif
    
    ///Try to compensate for the mistake
    
    *nodeHash_p = getHash();
    const U64& nodeHash = *nodeHash_p;
    boost::shared_ptr<RotoContext> roto =  getNode()->getRotoContext();
    if (roto) {
        *rotoAge_p = roto->getAge();
    } else {
        *rotoAge_p = 0;
    }
    
    {
        RECURSIVE_ACTION();
        Natron::StatusEnum stat = getRegionOfDefinition(nodeHash, time, scale, view, rod_p);
        if (stat == eStatusFailed) {
            return false;
        }
    }
    const RectD& rod = *rod_p;
    
    ///OptionalBoundsParam is the optional rectangle passed to getImage which may be NULL, in which case we use the RoD.
    if (!optionalBoundsParam) {
        ///// We cannot recover the RoI, we just assume the plug-in wants to render the full RoD.
        *optionalBounds_p = rod;
        ifInfiniteApplyHeuristic(nodeHash, time, scale, view, optionalBounds_p);
        const RectD& optionalBounds = *optionalBounds_p;
        
        /// If the region parameter is not set to NULL, then it will be clipped to the clip's
        /// Region of Definition for the given time. The returned image will be m at m least as big as this region.
        /// If the region parameter is not set, then the region fetched will be at least the Region of Interest
        /// the effect has previously specified, clipped the clip's Region of Definition.
        /// (renderRoI will do the clipping for us).
        
        
        ///// This code is wrong but executed ONLY IF THE PLUG-IN DOESN'T RESPECT THE SPECIFICATIONS. Recursive actions
        ///// should never happen.
        getRegionsOfInterest(time, scale, optionalBounds, optionalBounds, 0,inputRois_p);
    }
    
    assert( !( (supportsRenderScaleMaybe() == eSupportsNo) && !(scale.x == 1. && scale.y == 1.) ) );
    try {
        *isIdentity_p = isIdentity_public(nodeHash, time, scale, rod, getPreferredAspectRatio(), view, identityTime, identityInputNb_p);
    } catch (...) {
        return false;
    }


    return true;
}

void
EffectInstance::getThreadLocalInputImages(std::list<boost::shared_ptr<Natron::Image> >* images) const
{
    if (_imp->inputImages.hasLocalData()) {
        *images = _imp->inputImages.localData();
    }
}

bool
EffectInstance::getThreadLocalRegionsOfInterests(EffectInstance::RoIMap& roiMap) const
{
    if (!_imp->renderArgs.hasLocalData()) {
        return false;
    }
    RenderArgs& renderArgs = _imp->renderArgs.localData();
    if (!renderArgs._validArgs) {
        return false;
    }
    roiMap = renderArgs._regionOfInterestResults;
    return true;
}


boost::shared_ptr<Natron::Image>
EffectInstance::getImage(int inputNb,
                         const SequenceTime time,
                         const RenderScale & scale,
                         const int view,
                         const RectD *optionalBoundsParam, //!< optional region in canonical coordinates
                         const ImageComponents& comp,
                         const Natron::ImageBitDepthEnum depth,
                         const double par,
                         const bool dontUpscale,
                         RectI* roiPixel)
{
   
    ///The input we want the image from
    EffectInstance* n = getInput(inputNb);
    
    
    bool isMask = isInputMask(inputNb);
    
    if ( isMask && !isMaskEnabled(inputNb) ) {
        ///This is last resort, the plug-in should've checked getConnected() before, which would have returned false.
        return boost::shared_ptr<Natron::Image>();
    }
    boost::shared_ptr<RotoContext> roto = getNode()->getRotoContext();
    bool useRotoInput = false;
    if (roto) {
        useRotoInput = isInputRotoBrush(inputNb);
    }
    if ( ( !roto || (roto && !useRotoInput) ) && !n ) {
        return boost::shared_ptr<Natron::Image>();
    }
    
    RectD optionalBounds;
    if (optionalBoundsParam) {
        optionalBounds = *optionalBoundsParam;
    }
    unsigned int mipMapLevel = Image::getLevelFromScale(scale.x);
    RoIMap inputsRoI;
    RectD rod;
    bool isIdentity;
    int inputNbIdentity;
    int inputIdentityTime;
    U64 nodeHash;
    U64 rotoAge;
    
    /// Never by-pass the cache here because we already computed the image in renderRoI and by-passing the cache again can lead to
    /// re-computing of the same image many many times
    bool byPassCache = false;
    
    ///The caller thread MUST be a thread owned by Natron. It cannot be a thread from the multi-thread suite.
    ///A call to getImage is forbidden outside an action running in a thread launched by Natron.
    
    /// From http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#ImageEffectsImagesAndClipsUsingClips
    //    Images may be fetched from an attached clip in the following situations...
    //    in the kOfxImageEffectActionRender action
    //    in the kOfxActionInstanceChanged and kOfxActionEndInstanceChanged actions with a kOfxPropChangeReason of kOfxChangeUserEdited
    
    if ( !_imp->renderArgs.hasLocalData() || !_imp->frameRenderArgs.hasLocalData() ) {
        
        if ( !retrieveGetImageDataUponFailure(time, view, scale, optionalBoundsParam, &nodeHash, &rotoAge, &isIdentity, &inputIdentityTime, &inputNbIdentity, &rod, &inputsRoI, &optionalBounds) ) {
            return boost::shared_ptr<Image>();
        }
        
    } else {
        
        RenderArgs& renderArgs = _imp->renderArgs.localData();
        ParallelRenderArgs& frameRenderArgs = _imp->frameRenderArgs.localData();
        
        if (!renderArgs._validArgs || !frameRenderArgs.validArgs) {
            if ( !retrieveGetImageDataUponFailure(time, view, scale, optionalBoundsParam, &nodeHash, &rotoAge, &isIdentity, &inputIdentityTime, &inputNbIdentity, &rod, &inputsRoI, &optionalBounds) ) {
                return boost::shared_ptr<Image>();
            }
            
        } else {
            inputsRoI = renderArgs._regionOfInterestResults;
            rod = renderArgs._rod;
            isIdentity = renderArgs._isIdentity;
            inputIdentityTime = renderArgs._identityTime;
            inputNbIdentity = renderArgs._identityInputNb;
            nodeHash = frameRenderArgs.nodeHash;
            rotoAge = frameRenderArgs.rotoAge;
        }
        
        
    }
    
    RectD roi;
    if (!optionalBoundsParam) {
        RoIMap::iterator found = inputsRoI.find(useRotoInput ? this : n);
        if ( found != inputsRoI.end() ) {
            ///RoI is in canonical coordinates since the results of getRegionsOfInterest is in canonical coords.
            roi = found->second;
        } else {
            ///Oops, we didn't find the roi in the thread-storage... use  the RoD instead...
            roi = rod;
        }
    } else {
        roi = optionalBounds;
    }
    
    
    
    if ( isIdentity) {
        assert(inputNbIdentity != -2);
        ///If the effect is an identity but it didn't ask for the effect's image of which it is identity
        ///return a null image
        if (inputNbIdentity != inputNb) {
            return boost::shared_ptr<Image>();
        }
        
    }
    
    
    bool renderFullScaleThenDownscale = (!supportsRenderScale() && mipMapLevel != 0);
    ///Do we want to render the graph upstream at scale 1 or at the requested render scale ? (user setting)
    bool renderScaleOneUpstreamIfRenderScaleSupportDisabled = false;
    unsigned int renderMappedMipMapLevel = mipMapLevel;
    if (renderFullScaleThenDownscale) {
        renderScaleOneUpstreamIfRenderScaleSupportDisabled = getNode()->useScaleOneImagesWhenRenderScaleSupportIsDisabled();
        if (renderScaleOneUpstreamIfRenderScaleSupportDisabled) {
            renderMappedMipMapLevel = 0;
        }
    }
    
    ///Both the result of getRegionsOfInterest and optionalBounds are in canonical coordinates, we have to convert in both cases
    ///Convert to pixel coordinates
    RectI pixelRoI;
    roi.toPixelEnclosing(renderScaleOneUpstreamIfRenderScaleSupportDisabled ? 0 : mipMapLevel, par, &pixelRoI);
    
    //Try to find in the input images thread local storage if we already pre-computed the image
    std::list<boost::shared_ptr<Image> > inputImagesThreadLocal;
    if (_imp->inputImages.hasLocalData()) {
        inputImagesThreadLocal = _imp->inputImages.localData();
    }
    
    
    int channelForAlpha = !isMask ? -1 : getMaskChannel(inputNb);
    
    if (useRotoInput) {
        
        std::list<Natron::ImageComponents> outputComps;
        Natron::ImageBitDepthEnum outputDepth;
        getPreferredDepthAndComponents(-1, &outputComps, &outputDepth);
        
        //the roto input can only output color plane
        assert(outputComps.size() == 1 && outputComps.front().isColorPlane());
        
        boost::shared_ptr<Natron::Image> mask =  roto->renderMask(true,pixelRoI, outputComps.front(), nodeHash,rotoAge,
                                                                  rod, time, depth, view, mipMapLevel, inputImagesThreadLocal, byPassCache);
        if (inputImagesThreadLocal.empty()) {
            ///If the effect is analysis (e.g: Tracker) there's no input images in the tread local storage, hence add it
            _imp->addInputImageTempPointer(mask);
        }
        if (roiPixel) {
            *roiPixel = pixelRoI;
        }
        return mask;
    }
    
    
    //if the node is not connected, return a NULL pointer!
    if (!n) {
        return boost::shared_ptr<Natron::Image>();
    }
    
    
    std::list<ImageComponents> requestedComps;
    requestedComps.push_back(comp);
    ImageList inputImages;
    RenderRoIRetCode retCode = n->renderRoI( RenderRoIArgs(time,
                                                           scale,
                                                           renderMappedMipMapLevel,
                                                           view,
                                                           byPassCache,
                                                           pixelRoI,
                                                           RectD(),
                                                           requestedComps,
                                                           depth,
                                                           channelForAlpha,
                                                           true,
                                                           inputImagesThreadLocal), &inputImages );
    
    if (inputImages.empty() || retCode != eRenderRoIRetCodeOk) {
        return ImagePtr();
    }
    assert(inputImages.size() == 1);
    
    const ImagePtr& inputImg = inputImages.front();
    
    ///Check that the rendered image contains what we requested.
    assert(inputImg->getComponents() == comp);
    
    if (roiPixel) {
        *roiPixel = pixelRoI;
    }
    unsigned int inputImgMipMapLevel = inputImg->getMipMapLevel();
    
    if (inputImg->getPixelAspectRatio() != par) {
        qDebug() << "WARNING: " << getScriptName_mt_safe().c_str() << " requested an image with a pixel aspect ratio of " << par <<
        " but " << n->getScriptName_mt_safe().c_str() << " rendered an image with a pixel aspect ratio of " << inputImg->getPixelAspectRatio();
    }
    
    ///If the plug-in doesn't support the render scale, but the image is downscaled, up-scale it.
    ///Note that we do NOT cache it because it is really low def!
    if ( !dontUpscale  && renderFullScaleThenDownscale && inputImgMipMapLevel != 0 ) {
        assert(inputImgMipMapLevel != 0);
        ///Resize the image according to the requested scale
        Natron::ImageBitDepthEnum bitdepth = inputImg->getBitDepth();
        RectI bounds;
        inputImg->getRoD().toPixelEnclosing(0, par, &bounds);
        boost::shared_ptr<Natron::Image> rescaledImg( new Natron::Image(inputImg->getComponents(), inputImg->getRoD(),
                                                                        bounds, 0, par, bitdepth) );
        inputImg->upscaleMipMap(inputImg->getBounds(), inputImgMipMapLevel, 0, rescaledImg.get());
        if (roiPixel) {
            RectD canonicalPixelRoI;
            pixelRoI.toCanonical(inputImgMipMapLevel, par, rod, &canonicalPixelRoI);
            canonicalPixelRoI.toPixelEnclosing(0, par, roiPixel);
        }
        //inputImg->scaleBox( inputImg->getBounds(), rescaledImg.get() );
        return rescaledImg;
        
        
    } else {
        
        if (inputImagesThreadLocal.empty()) {
            ///If the effect is analysis (e.g: Tracker) there's no input images in the tread local storage, hence add it
            _imp->addInputImageTempPointer(inputImg);
        }
        return inputImg;
        
    }
    
} // getImage 

void
EffectInstance::calcDefaultRegionOfDefinition(U64 /*hash*/,SequenceTime /*time*/,
                                              int /*view*/,
                                              const RenderScale & /*scale*/,
                                              RectD *rod)
{
    Format projectDefault;

    getRenderFormat(&projectDefault);
    *rod = RectD( projectDefault.left(), projectDefault.bottom(), projectDefault.right(), projectDefault.top() );
}

Natron::StatusEnum
EffectInstance::getRegionOfDefinition(U64 hash,SequenceTime time,
                                      const RenderScale & scale,
                                      int view,
                                      RectD* rod) //!< rod is in canonical coordinates
{
    bool firstInput = true;
    RenderScale renderMappedScale = scale;

    assert( !( (supportsRenderScaleMaybe() == eSupportsNo) && !(scale.x == 1. && scale.y == 1.) ) );

    for (int i = 0; i < getMaxInputCount(); ++i) {
        Natron::EffectInstance* input = getInput(i);
        if (input) {
            RectD inputRod;
            bool isProjectFormat;
            StatusEnum st = input->getRegionOfDefinition_public(hash,time, renderMappedScale, view, &inputRod, &isProjectFormat);
            assert(inputRod.x2 >= inputRod.x1 && inputRod.y2 >= inputRod.y1);
            if (st == eStatusFailed) {
                return st;
            }

            if (firstInput) {
                *rod = inputRod;
                firstInput = false;
            } else {
                rod->merge(inputRod);
            }
            assert(rod->x1 <= rod->x2 && rod->y1 <= rod->y2);
        }
    }

    return eStatusReplyDefault;
}

bool
EffectInstance::ifInfiniteApplyHeuristic(U64 hash,
                                         SequenceTime time,
                                         const RenderScale & scale,
                                         int view,
                                         RectD* rod) //!< input/output
{
    /*If the rod is infinite clip it to the project's default*/

    Format projectFormat;
    getRenderFormat(&projectFormat);
    RectD projectDefault = projectFormat.toCanonicalFormat();
    /// FIXME: before removing the assert() (I know you are tempted) please explain (here: document!) if the format rectangle can be empty and in what situation(s)
    assert( !projectDefault.isNull() );

    assert(rod);
    assert(rod->x1 <= rod->x2 && rod->y1 <= rod->y2);
    bool x1Infinite = rod->x1 <= kOfxFlagInfiniteMin;
    bool y1Infinite = rod->y1 <= kOfxFlagInfiniteMin;
    bool x2Infinite = rod->x2 >= kOfxFlagInfiniteMax;
    bool y2Infinite = rod->y2 >= kOfxFlagInfiniteMax;

    ///Get the union of the inputs.
    RectD inputsUnion;

    ///Do the following only if one coordinate is infinite otherwise we wont need the RoD of the input
    if (x1Infinite || y1Infinite || x2Infinite || y2Infinite) {
        // initialize with the effect's default RoD, because inputs may not be connected to other effects (e.g. Roto)
        calcDefaultRegionOfDefinition(hash,time,view, scale, &inputsUnion);
        bool firstInput = true;
        for (int i = 0; i < getMaxInputCount(); ++i) {
            Natron::EffectInstance* input = getInput(i);
            if (input) {
                RectD inputRod;
                bool isProjectFormat;
                RenderScale inputScale = scale;
                if (input->supportsRenderScaleMaybe() == eSupportsNo) {
                    inputScale.x = inputScale.y = 1.;
                }
                StatusEnum st = input->getRegionOfDefinition_public(hash,time, inputScale, view, &inputRod, &isProjectFormat);
                if (st != eStatusFailed) {
                    if (firstInput) {
                        inputsUnion = inputRod;
                        firstInput = false;
                    } else {
                        inputsUnion.merge(inputRod);
                    }
                }
            }
        }
    }
    ///If infinite : clip to inputsUnion if not null, otherwise to project default

    // BE CAREFUL:
    // std::numeric_limits<int>::infinity() does not exist (check std::numeric_limits<int>::has_infinity)
    bool isProjectFormat = false;
    if (x1Infinite) {
        if ( !inputsUnion.isNull() ) {
            rod->x1 = std::min(inputsUnion.x1, projectDefault.x1);
        } else {
            rod->x1 = projectDefault.x1;
            isProjectFormat = true;
        }
        rod->x2 = std::max(rod->x1, rod->x2);
    }
    if (y1Infinite) {
        if ( !inputsUnion.isNull() ) {
            rod->y1 = std::min(inputsUnion.y1, projectDefault.y1);
        } else {
            rod->y1 = projectDefault.y1;
            isProjectFormat = true;
        }
        rod->y2 = std::max(rod->y1, rod->y2);
    }
    if (x2Infinite) {
        if ( !inputsUnion.isNull() ) {
            rod->x2 = std::max(inputsUnion.x2, projectDefault.x2);
        } else {
            rod->x2 = projectDefault.x2;
            isProjectFormat = true;
        }
        rod->x1 = std::min(rod->x1, rod->x2);
    }
    if (y2Infinite) {
        if ( !inputsUnion.isNull() ) {
            rod->y2 = std::max(inputsUnion.y2, projectDefault.y2);
        } else {
            rod->y2 = projectDefault.y2;
            isProjectFormat = true;
        }
        rod->y1 = std::min(rod->y1, rod->y2);
    }
    if ( isProjectFormat && !isGenerator() ) {
        isProjectFormat = false;
    }
    assert(rod->x1 <= rod->x2 && rod->y1 <= rod->y2);

    return isProjectFormat;
} // ifInfiniteApplyHeuristic

void
EffectInstance::getRegionsOfInterest(SequenceTime /*time*/,
                                     const RenderScale & /*scale*/,
                                     const RectD & /*outputRoD*/, //!< the RoD of the effect, in canonical coordinates
                                     const RectD & renderWindow, //!< the region to be rendered in the output image, in Canonical Coordinates
                                     int /*view*/,
                                     EffectInstance::RoIMap* ret)
{
    for (int i = 0; i < getMaxInputCount(); ++i) {
        Natron::EffectInstance* input = getInput(i);
        if (input) {
            ret->insert( std::make_pair(input, renderWindow) );
        }
    }
}

EffectInstance::FramesNeededMap
EffectInstance::getFramesNeeded(SequenceTime time, int view)
{
    EffectInstance::FramesNeededMap ret;
    RangeD defaultRange;
    
    defaultRange.min = defaultRange.max = time;
    std::vector<RangeD> ranges;
    ranges.push_back(defaultRange);
    std::map<int,std::vector<RangeD> > defViewRange;
    defViewRange.insert(std::make_pair(view, ranges));
    for (int i = 0; i < getMaxInputCount(); ++i) {
        if (isInputRotoBrush(i)) {
            ret.insert( std::make_pair(i, defViewRange) );
        } else {
            Natron::EffectInstance* input = getInput(i);
            if (input) {
                ret.insert( std::make_pair(i, defViewRange) );
            }
        }
    }

    return ret;
}

void
EffectInstance::getFrameRange(SequenceTime *first,
                              SequenceTime *last)
{
    // default is infinite if there are no non optional input clips
    *first = INT_MIN;
    *last = INT_MAX;
    for (int i = 0; i < getMaxInputCount(); ++i) {
        Natron::EffectInstance* input = getInput(i);
        if (input) {
            SequenceTime inpFirst,inpLast;
            input->getFrameRange(&inpFirst, &inpLast);
            if (i == 0) {
                *first = inpFirst;
                *last = inpLast;
            } else {
                if (inpFirst < *first) {
                    *first = inpFirst;
                }
                if (inpLast > *last) {
                    *last = inpLast;
                }
            }
        }
    }
}

EffectInstance::NotifyRenderingStarted_RAII::NotifyRenderingStarted_RAII(Node* node)
: _node(node)
{
    _didEmit = node->notifyRenderingStarted();
}

EffectInstance::NotifyRenderingStarted_RAII::~NotifyRenderingStarted_RAII()
{
    if (_didEmit) {
        _node->notifyRenderingEnded();
    }
}

EffectInstance::NotifyInputNRenderingStarted_RAII::NotifyInputNRenderingStarted_RAII(Node* node,int inputNumber)
: _node(node)
, _inputNumber(inputNumber)
{
    _didEmit = node->notifyInputNIsRendering(inputNumber);
}

EffectInstance::NotifyInputNRenderingStarted_RAII::~NotifyInputNRenderingStarted_RAII()
{
    if (_didEmit) {
        _node->notifyInputNIsFinishedRendering(_inputNumber);
    }
}

static void getOrCreateFromCacheInternal(const ImageKey& key,
                                         const boost::shared_ptr<ImageParams>& params,
                                         bool useCache,
                                         bool useDiskCache,
                                         ImagePtr* image)
{
    
    
    if (useCache) {
        !useDiskCache ? Natron::getImageFromCacheOrCreate(key, params, image) :
        Natron::getImageFromDiskCacheOrCreate(key, params, image);
        
        if (!*image) {
            std::stringstream ss;
            ss << "Failed to allocate an image of ";
            ss << printAsRAM( params->getElementsCount() * sizeof(Image::data_t) ).toStdString();
            Natron::errorDialog( QObject::tr("Out of memory").toStdString(),ss.str() );
            return;
        }
    
        /*
         * Note that at this point the image is already exposed to other threads and another one might already have allocated it.
         * This function does nothing if it has been reallocated already.
         */
        (*image)->allocateMemory();
        
        
        
        /*
         * Another thread might have allocated the same image in the cache but with another RoI, make sure
         * it is big enough for us, or resize it to our needs.
         */
        

        (*image)->ensureBounds(params->getBounds());
        
        
    } else {
        image->reset(new Image(key, params));
    }
}

void
EffectInstance::getImageFromCacheAndConvertIfNeeded(bool useCache,
                                                    bool useDiskCache,
                                                    const Natron::ImageKey& key,
                                                    unsigned int mipMapLevel,
                                                    const RectI& bounds,
                                                    const RectD& rod,
                                                    Natron::ImageBitDepthEnum bitdepth,
                                                    const Natron::ImageComponents& components,
                                                    Natron::ImageBitDepthEnum nodePrefDepth,
                                                    const Natron::ImageComponents& nodePrefComps,
                                                    const std::list<boost::shared_ptr<Natron::Image> >& inputImages,
                                                    boost::shared_ptr<Natron::Image>* image)
{
    ImageList cachedImages;
    bool isCached = false;
    
    ///Find first something in the input images list
    if (!inputImages.empty()) {
        for (std::list<boost::shared_ptr<Image> >::const_iterator it = inputImages.begin(); it != inputImages.end(); ++it) {
            if (!it->get()) {
                continue;
            }
            const ImageKey& imgKey = (*it)->getKey();
            if (imgKey == key) {
                cachedImages.push_back(*it);
                isCached = true;
            }
        }
    }
    
    if (!isCached) {
        isCached = !useDiskCache ? Natron::getImageFromCache(key,&cachedImages) : Natron::getImageFromDiskCache(key, &cachedImages);
    }
    
    if (isCached) {
        
        ///A ptr to a higher resolution of the image or an image with different comps/bitdepth
        ImagePtr imageToConvert;
        
        for (ImageList::iterator it = cachedImages.begin(); it!=cachedImages.end(); ++it) {
            unsigned int imgMMlevel = (*it)->getMipMapLevel();
            const Natron::ImageComponents& imgComps = (*it)->getComponents();
            ImageBitDepthEnum imgDepth = (*it)->getBitDepth();
            
            if ( (*it)->getParams()->isRodProjectFormat() ) {
                ////If the image was cached with a RoD dependent on the project format, but the project format changed,
                ////just discard this entry
                Format projectFormat;
                getRenderFormat(&projectFormat);
                RectD canonicalProject = projectFormat.toCanonicalFormat();
                if ( canonicalProject != (*it)->getRoD() ) {
                    appPTR->removeFromNodeCache(*it);
                    continue;
                }
            }
            
            ///Throw away images that are not even what the node want to render
            if ((imgComps.isColorPlane() && nodePrefComps.isColorPlane() && imgComps != nodePrefComps) || imgDepth != nodePrefDepth) {
                appPTR->removeFromNodeCache(*it);
                continue;
            }
            

            if (imgMMlevel == mipMapLevel && imgComps.isConvertibleTo(components) &&
            getSizeOfForBitDepth(imgDepth) >= getSizeOfForBitDepth(bitdepth)/* && imgComps == components && imgDepth == bitdepth*/) {
                
                ///We found  a matching image
                
                *image = *it;
                break;
            } else {
                
                
                if (imgMMlevel >= mipMapLevel || !imgComps.isConvertibleTo(components) ||
                    getSizeOfForBitDepth(imgDepth) < getSizeOfForBitDepth(bitdepth)) {
                    ///Either smaller resolution or not enough components or bit-depth is not as deep, don't use the image
                    continue;
                }
                
                assert(imgMMlevel < mipMapLevel);
                
                if (!imageToConvert) {
                    imageToConvert = *it;

                } else {
                    ///We found an image which scale is closer to the requested mipmap level we want, use it instead
                    if (imgMMlevel > imageToConvert->getMipMapLevel()) {
                        imageToConvert = *it;
                    }
                }
                
                
            }
        } //end for
        
        if (imageToConvert && !*image) {

            ///Ensure the image is allocated
            (imageToConvert)->allocateMemory();

            
            if (imageToConvert->getMipMapLevel() != mipMapLevel) {
                boost::shared_ptr<ImageParams> oldParams = imageToConvert->getParams();
                
                assert(imageToConvert->getMipMapLevel() < mipMapLevel);
                
                RectI imgToConvertBounds = imageToConvert->getBounds();
                RectD imgToConvertCanonical;
                imgToConvertBounds.toCanonical(imageToConvert->getMipMapLevel(), imageToConvert->getPixelAspectRatio(), rod, &imgToConvertCanonical);
                RectI downscaledBounds;
                
                imgToConvertCanonical.toPixelEnclosing(imageToConvert->getMipMapLevel(), imageToConvert->getPixelAspectRatio(), &imgToConvertBounds);
                imgToConvertCanonical.toPixelEnclosing(mipMapLevel, imageToConvert->getPixelAspectRatio(), &downscaledBounds);
                
                downscaledBounds.merge(bounds);
                
                RectI pixelRoD;
                rod.toPixelEnclosing(mipMapLevel, oldParams->getPixelAspectRatio(), &pixelRoD);
                downscaledBounds.intersect(pixelRoD, &downscaledBounds);
                
                boost::shared_ptr<ImageParams> imageParams = Image::makeParams(oldParams->getCost(),
                                                                               rod,
                                                                               downscaledBounds,
                                                                               oldParams->getPixelAspectRatio(),
                                                                               mipMapLevel,
                                                                               oldParams->isRodProjectFormat(),
                                                                               oldParams->getComponents(),
                                                                               oldParams->getBitDepth(),
                                                                               oldParams->getFramesNeeded());
                
                
                imageParams->setMipMapLevel(mipMapLevel);
                
                
                boost::shared_ptr<Image> img;
                getOrCreateFromCacheInternal(key,imageParams,useCache,useDiskCache,&img);
                if (!img) {
                    return;
                }
                
                imageToConvert->downscaleMipMap(rod,
                                                imgToConvertBounds,
                                                imageToConvert->getMipMapLevel(), img->getMipMapLevel() ,
                                                useCache && imageToConvert->usesBitMap(),
                                                img.get());
                
                imageToConvert = img;
            

                imageToConvert->ensureBounds(bounds);
                


            }
            
            *image = imageToConvert;
            assert(imageToConvert->getBounds().contains(bounds));
            
        } else if (*image) { //  else if (imageToConvert && !*image)
            
            ///Ensure the image is allocated
            (*image)->allocateMemory();
            
            
            /*
             * Another thread might have allocated the same image in the cache but with another RoI, make sure
             * it is big enough for us, or resize it to our needs.
             */
   
            (*image)->ensureBounds(bounds);
            assert((*image)->getBounds().contains(bounds));

        }
        
    }

}

void
EffectInstance::tryConcatenateTransforms(const RenderRoIArgs& args,
                                         std::list<InputMatrix>* inputTransforms)
{
    
    bool canTransform = getCanTransform();
    
    //An effect might not be able to concatenate transforms but can still apply a transform (e.g CornerPinMasked)
    std::list<int> inputHoldingTransforms;
    bool canApplyTransform = getInputsHoldingTransform(&inputHoldingTransforms);
    assert(inputHoldingTransforms.empty() || canApplyTransform);
    
    Transform::Matrix3x3 thisNodeTransform;
    Natron::EffectInstance* inputToTransform = 0;
    
    bool getTransformSucceeded = false;
    
    if (canTransform) {
        /*
         * If getting the transform does not succeed, then this effect is treated as any other ones.
         */
        assert(canApplyTransform);
        Natron::StatusEnum stat = getTransform_public(args.time, args.scale, args.view, &inputToTransform, &thisNodeTransform);
        if (stat == eStatusOK) {
            getTransformSucceeded = true;
        }
    }

    
    if ((canTransform && getTransformSucceeded) || (!canTransform && canApplyTransform && !inputHoldingTransforms.empty())) {
        
        assert(!inputHoldingTransforms.empty());
      
        for (std::list<int>::iterator it = inputHoldingTransforms.begin(); it!=inputHoldingTransforms.end(); ++it) {
            
            EffectInstance* input = getInput(*it);
            if (!input) {
                continue;
            }
            std::list<Transform::Matrix3x3> matricesByOrder; // from downstream to upstream

            InputMatrix im;
            im.inputNb = *it;
            im.newInputEffect = input;
            im.newInputNbToFetchFrom = im.inputNb;

            
            // recursion upstream
            bool inputCanTransform = false;
            bool inputIsDisabled  =  input->getNode()->isNodeDisabled();
            
            if (!inputIsDisabled) {
                inputCanTransform = input->getCanTransform();
            }
            
            
            while (input && (inputCanTransform || inputIsDisabled)) {
                
                //input is either disabled, or identity or can concatenate a transform too
                if (inputIsDisabled) {
                    
                    int prefInput;
                    input = input->getNearestNonDisabledPrevious(&prefInput);
                    if (prefInput == -1) {
                        break;
                    }
                    
                    if (input) {
                        im.newInputNbToFetchFrom = prefInput;
                        im.newInputEffect = input;
                    }
                    
                } else if (inputCanTransform) {
                    Transform::Matrix3x3 m;
                    inputToTransform = 0;
                    Natron::StatusEnum stat = input->getTransform_public(args.time, args.scale, args.view, &inputToTransform, &m);
                    if (stat == eStatusOK) {
                        matricesByOrder.push_back(m);
                        if (inputToTransform) {
                            im.newInputNbToFetchFrom = input->getInputNumber(inputToTransform);
                            im.newInputEffect = input;
                            input = inputToTransform;
                        }
                    } else {
                        break;
                    }
                } else  {
                    assert(false);
                }
                
                
                if (input) {
                    inputIsDisabled = input->getNode()->isNodeDisabled();
                    if (!inputIsDisabled) {
                        inputCanTransform = input->getCanTransform();
                    }
                }
            }
            
            
            if (input && !matricesByOrder.empty()) {
                
                assert(im.newInputEffect);
                
                ///Now actually concatenate matrices together
                im.cat.reset(new Transform::Matrix3x3);
                std::list<Transform::Matrix3x3>::iterator it2 = matricesByOrder.begin();
                *im.cat = *it2;
                ++it2;
                while (it2 != matricesByOrder.end()) {
                    *im.cat = Transform::matMul(*im.cat, *it2);
                    ++it2;
                }
                
                inputTransforms->push_back(im);
            }
            
        } //  for (std::list<int>::iterator it = inputHoldingTransforms.begin(); it!=inputHoldingTransforms.end(); ++it)

    } // if ((canTransform && getTransformSucceeded) || (canApplyTransform && !inputHoldingTransforms.empty()))

}

class TransformReroute_RAII
{
    EffectInstance* self;
    std::list<EffectInstance::InputMatrix> transforms;
public:
    
    TransformReroute_RAII(EffectInstance* self,const std::list<EffectInstance::InputMatrix>& inputTransforms)
    : self(self)
    , transforms(inputTransforms)
    {
        self->rerouteInputAndSetTransform(inputTransforms);
    }
    
    ~TransformReroute_RAII()
    {
        for (std::list<EffectInstance::InputMatrix>::iterator it = transforms.begin(); it!=transforms.end(); ++it) {
            self->clearTransform(it->inputNb);
        }
        
    }
};


bool
EffectInstance::allocateImagePlane(const ImageKey& key,
                                   const RectD& rod,
                                   const RectI& downscaleImageBounds,
                                   const RectI& fullScaleImageBounds,
                                   bool isProjectFormat,
                                   const FramesNeededMap& framesNeeded,
                                   const Natron::ImageComponents& components,
                                   Natron::ImageBitDepthEnum depth,
                                   double par,
                                   unsigned int mipmapLevel,
                                   bool renderFullScaleThenDownscale,
                                   bool renderScaleOneUpstreamIfRenderScaleSupportDisabled,
                                   bool useDiskCache,
                                   bool createInCache,
                                   boost::shared_ptr<Natron::Image>* fullScaleImage,
                                   boost::shared_ptr<Natron::Image>* downscaleImage)
{
    //Controls whether images are stored on disk or in RAM, 0 = RAM, 1 = mmap
    int cost = useDiskCache ? 1 : 0;
    
    //If we're rendering full scale and with input images at full scale, don't cache the downscale image since it is cheap to
    //recreate, instead cache the full-scale image
    if (renderFullScaleThenDownscale && renderScaleOneUpstreamIfRenderScaleSupportDisabled) {
        
        downscaleImage->reset( new Natron::Image(components, rod, downscaleImageBounds, mipmapLevel, par, depth, true) );
        
    } else {
        
        ///Cache the image with the requested components instead of the remapped ones
        boost::shared_ptr<Natron::ImageParams> cachedImgParams = Natron::Image::makeParams(cost,
                                                                                           rod,
                                                                                           downscaleImageBounds,
                                                                                           par,
                                                                                           mipmapLevel,
                                                                                           isProjectFormat,
                                                                                           components,
                                                                                           depth,
                                                                                           framesNeeded);
        
        //Take the lock after getting the image from the cache or while allocating it
        ///to make sure a thread will not attempt to write to the image while its being allocated.
        ///When calling allocateMemory() on the image, the cache already has the lock since it added it
        ///so taking this lock now ensures the image will be allocated completetly
        
        getOrCreateFromCacheInternal(key,cachedImgParams,createInCache,useDiskCache,fullScaleImage);
        if (!*fullScaleImage) {
            return false;
        }
        
        
        *downscaleImage = *fullScaleImage;
    }
    
    if (renderFullScaleThenDownscale) {
        
        if (!renderScaleOneUpstreamIfRenderScaleSupportDisabled) {
            
            ///The upscaled image will be rendered using input images at lower def... which means really crappy results, don't cache this image!
            fullScaleImage->reset( new Natron::Image(components, rod, fullScaleImageBounds, 0, par, depth, true) );
            
        } else {
            
            boost::shared_ptr<Natron::ImageParams> upscaledImageParams = Natron::Image::makeParams(cost,
                                                                                                   rod,
                                                                                                   fullScaleImageBounds,
                                                                                                   par,
                                                                                                   0,
                                                                                                   isProjectFormat,
                                                                                                   components,
                                                                                                   depth,
                                                                                                   framesNeeded);
            
            //The upscaled image will be rendered with input images at full def, it is then the best possibly rendered image so cache it!
            
            fullScaleImage->reset();
            getOrCreateFromCacheInternal(key,upscaledImageParams,createInCache,useDiskCache,fullScaleImage);
            
            if (!*fullScaleImage) {
                return false;
            }
        }
        
    }
    return true;
}



EffectInstance::RenderRoIRetCode EffectInstance::renderRoI(const RenderRoIArgs & args,ImageList* outputPlanes)
{
   
    //Do nothing if no components were requested
    if (args.components.empty()) {
        return eRenderRoIRetCodeFailed;
    }
    
    ParallelRenderArgs& frameRenderArgs = _imp->frameRenderArgs.localData();
    if (!frameRenderArgs.validArgs) {
        qDebug() << "Thread-storage for the render of the frame was not set, this is a bug.";
        frameRenderArgs.time = args.time;
        frameRenderArgs.nodeHash = getHash();
        frameRenderArgs.view = args.view;
        frameRenderArgs.isSequentialRender = false;
        frameRenderArgs.isRenderResponseToUserInteraction = true;
        boost::shared_ptr<RotoContext> roto = getNode()->getRotoContext();
        if (roto) {
            frameRenderArgs.rotoAge = roto->getAge();
        } else {
            frameRenderArgs.rotoAge = 0;
        }
        frameRenderArgs.validArgs = true;
    }
    
    ///The args must have been set calling setParallelRenderArgs
    assert(frameRenderArgs.validArgs);
    
    
    ///For writer we never want to cache otherwise the next time we want to render it will skip writing the image on disk!
    bool byPassCache = args.byPassCache;

    ///Use the hash at this time, and then copy it to the clips in the thread local storage to use the same value
    ///through all the rendering of this frame.
    U64 nodeHash = frameRenderArgs.nodeHash;
 
    const double par = getPreferredAspectRatio();


    RectD rod; //!< rod is in canonical coordinates
    bool isProjectFormat = false;
    const unsigned int mipMapLevel = args.mipMapLevel;
    SupportsEnum supportsRS = supportsRenderScaleMaybe();
    ///This flag is relevant only when the mipMapLevel is different than 0. We use it to determine
    ///wether the plug-in should render in the full scale image, and then we downscale afterwards or
    ///if the plug-in can just use the downscaled image to render.
    bool renderFullScaleThenDownscale = (supportsRS == eSupportsNo && mipMapLevel != 0);
    unsigned int renderMappedMipMapLevel;
    if (renderFullScaleThenDownscale) {
        renderMappedMipMapLevel = 0;
    } else {
        renderMappedMipMapLevel = args.mipMapLevel;
    }
    RenderScale renderMappedScale;
    renderMappedScale.x = renderMappedScale.y = Image::getScaleFromMipMapLevel(renderMappedMipMapLevel);
    assert( !( (supportsRS == eSupportsNo) && !(renderMappedScale.x == 1. && renderMappedScale.y == 1.) ) );

    ///Do we want to render the graph upstream at scale 1 or at the requested render scale ? (user setting)
    bool renderScaleOneUpstreamIfRenderScaleSupportDisabled = false;
    if (renderFullScaleThenDownscale) {

        renderScaleOneUpstreamIfRenderScaleSupportDisabled = getNode()->useScaleOneImagesWhenRenderScaleSupportIsDisabled();
        
        ///For multi-resolution we want input images with exactly the same size as the output image
        if (!renderScaleOneUpstreamIfRenderScaleSupportDisabled && !supportsMultiResolution()) {
            renderScaleOneUpstreamIfRenderScaleSupportDisabled = true;
        }
    }
    
 
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// Get the RoD ///////////////////////////////////////////////////////////////
    
    ///if the rod is already passed as parameter, just use it and don't call getRegionOfDefinition
    if ( !args.preComputedRoD.isNull() ) {
        rod = args.preComputedRoD;
    } else {
        ///before allocating it we must fill the RoD of the image we want to render
        assert( !( (supportsRS == eSupportsNo) && !(renderMappedScale.x == 1. && renderMappedScale.y == 1.) ) );
        StatusEnum stat = getRegionOfDefinition_public(nodeHash,args.time, renderMappedScale, args.view, &rod, &isProjectFormat);

        ///The rod might be NULL for a roto that has no beziers and no input
        if (stat == eStatusFailed) {
            ///if getRoD fails, this might be because the RoD is null after all (e.g: an empty Roto node), we don't want the render to fail
            return eRenderRoIRetCodeOk;
        } else if (rod.isNull()) {
            //Nothing to render
            return eRenderRoIRetCodeOk;
        }
        if ( (supportsRS == eSupportsMaybe) && (renderMappedMipMapLevel != 0) ) {
            // supportsRenderScaleMaybe may have changed, update it
            supportsRS = supportsRenderScaleMaybe();
            renderFullScaleThenDownscale = (supportsRS == eSupportsNo && mipMapLevel != 0);
            if (renderFullScaleThenDownscale) {
                renderMappedScale.x = renderMappedScale.y = 1.;
                renderMappedMipMapLevel = 0;
            }
        }
    }
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// End get RoD ///////////////////////////////////////////////////////////////

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// Check if effect is identity ///////////////////////////////////////////////////////////////
    {
        SequenceTime inputTimeIdentity = 0.;
        int inputNbIdentity;
        
        assert( !( (supportsRS == eSupportsNo) && !(renderMappedScale.x == 1. && renderMappedScale.y == 1.) ) );
        bool identity;
        try {
            identity = isIdentity_public(nodeHash,args.time, renderMappedScale, rod, par, args.view, &inputTimeIdentity, &inputNbIdentity);
        } catch (...) {
            return eRenderRoIRetCodeFailed;
        }
        
        if ( (supportsRS == eSupportsMaybe) && (renderMappedMipMapLevel != 0) ) {
            // supportsRenderScaleMaybe may have changed, update it
            supportsRS = supportsRenderScaleMaybe();
            renderFullScaleThenDownscale = (supportsRS == eSupportsNo && mipMapLevel != 0);
            if (renderFullScaleThenDownscale) {
                renderMappedScale.x = renderMappedScale.y = 1.;
                renderMappedMipMapLevel = 0;
            }
        }
        
        if (identity) {
            ///The effect is an identity but it has no inputs
            if (inputNbIdentity == -1) {
                return eRenderRoIRetCodeFailed;
            } else if (inputNbIdentity == -2) {
                // there was at least one crash if you set the first frame to a negative value
                assert(inputTimeIdentity != args.time);
                if (inputTimeIdentity != args.time) { // be safe in release mode!
                    ///This special value of -2 indicates that the plugin is identity of itself at another time
                    RenderRoIArgs argCpy = args;
                    argCpy.time = inputTimeIdentity;
                    argCpy.preComputedRoD.clear(); //< clear as the RoD of the identity input might not be the same (reproducible with Blur)
                    
                    return renderRoI(argCpy,outputPlanes);
                }
            }
            
            int firstFrame,lastFrame;
            getFrameRange_public(nodeHash, &firstFrame, &lastFrame);
            
            RectD canonicalRoI;
            ///WRONG! We can't clip against the RoD of *this* effect. We should clip against the RoD of the input effect, but this is done
            ///later on for us already.
            //args.roi.toCanonical(args.mipMapLevel, rod, &canonicalRoI);
            args.roi.toCanonical_noClipping(args.mipMapLevel, par,  &canonicalRoI);
            RoIMap inputsRoI;
            inputsRoI.insert( std::make_pair(getInput(inputNbIdentity), canonicalRoI) );
            Implementation::ScopedRenderArgs scopedArgs(&_imp->renderArgs,
                                                        inputsRoI,
                                                        rod,
                                                        args.roi,
                                                        args.time,
                                                        args.view,
                                                        args.channelForAlpha,
                                                        identity,
                                                        inputTimeIdentity,
                                                        inputNbIdentity,
                                                        std::map<Natron::ImageComponents,PlaneToRender>(),
                                                        firstFrame,
                                                        lastFrame);
            Natron::EffectInstance* inputEffectIdentity = getInput(inputNbIdentity);
            
            if (inputEffectIdentity) {
                ///we don't need to call getRegionOfDefinition and getFramesNeeded if the effect is an identity
                RenderRoIArgs inputArgs = args;
                inputArgs.time = inputTimeIdentity;
                
                return inputEffectIdentity->renderRoI(inputArgs, outputPlanes);

            }
            
            return eRenderRoIRetCodeFailed;
            
        } // if (identity)
    }
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// End identity check ///////////////////////////////////////////////////////////////
    
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// Handle pass-through for planes //////////////////////////////////////////////////////////
    ComponentsAvailableMap componentsAvailables;
    
    //Available planes/components is view agnostic
    getComponentsAvailable(args.time, &componentsAvailables);
    
    /*
     * For all requested planes, check which components can be produced in output by this node. 
     * If the components are from the color plane, if another set of components of the color plane is present
     * we try to render with those instead.
     */
    std::list<Natron::ImageComponents> requestedComponents;
    
    ComponentsAvailableMap componentsToFetchUpstream;
    for (std::list<Natron::ImageComponents>::const_iterator it = args.components.begin(); it != args.components.end(); ++it) {
        
        assert(it->getNumComponents() > 0);
        
        bool isColorComponents = it->isColorPlane();;
        
        ComponentsAvailableMap::iterator found = componentsAvailables.end();
        for (ComponentsAvailableMap::iterator it2 = componentsAvailables.begin(); it2 != componentsAvailables.end(); ++it2) {
            if (it2->first == *it) {
                found = it2;
                break;
            } else {
                if (isColorComponents && it2->first.isColorPlane() && isSupportedComponent(-1, it2->first)) {
                    //We found another set of components in the color plane, take it
                    found = it2;
                    break;
                }
            }
        }
        
        // If  the requested component is not present, then it will just return black and transparant to the plug-in.
        if (found != componentsAvailables.end()) {
            if (found->second.lock() == getNode()) {
                requestedComponents.push_back(*it);
            } else {
                //The component is not available directly from this node, fetch it upstream
                componentsToFetchUpstream.insert(std::make_pair(*it, found->second.lock()));
            }
            
        }
    }
    
    //Render planes that we are not able to render on this node from upstream
    for (ComponentsAvailableMap::iterator it = componentsToFetchUpstream.begin(); it != componentsToFetchUpstream.end(); ++it) {
        NodePtr node = it->second.lock();
        if (node) {
            RenderRoIArgs inArgs = args;
            inArgs.components.clear();
            inArgs.components.push_back(it->first);
            ImageList inputPlanes;
            RenderRoIRetCode inputRetCode = node->getLiveInstance()->renderRoI(inArgs,&inputPlanes);
            assert(inputPlanes.size() == 1 || inputPlanes.empty());
            if (inputRetCode == eRenderRoIRetCodeAborted || inputRetCode == eRenderRoIRetCodeFailed || inputPlanes.empty()) {
                return inputRetCode;
            }
            outputPlanes->push_back(inputPlanes.front());
        }
    }
    
    ///There might be only planes to render that were fetched from upstream
    if (requestedComponents.empty()) {
        return eRenderRoIRetCodeFailed;
    }
    
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// End pass-through for planes //////////////////////////////////////////////////////////

    
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// Transform concatenations ///////////////////////////////////////////////////////////////
    ///Try to concatenate transform effects
    std::list<InputMatrix> inputsToTransform;
    if (appPTR->getCurrentSettings()->isTransformConcatenationEnabled()) {
        tryConcatenateTransforms(args, &inputsToTransform);
    }
    
    ///Ok now we have the concatenation of all matrices, set it on the associated clip and reroute the tree
    boost::shared_ptr<TransformReroute_RAII> transformConcatenationReroute;
    if (!inputsToTransform.empty()) {
        transformConcatenationReroute.reset(new TransformReroute_RAII(this,inputsToTransform));
    }
    
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /////////////////////////////////End transform concatenations//////////////////////////////////////////////////////////
    
    /*We pass the 2 images (image & downscaledImage). Depending on the context we want to render in one or the other one:
     If (renderFullScaleThenDownscale and renderScaleOneUpstreamIfRenderScaleSupportDisabled)
     the image that is held by the cache will be 'image' and it will then be downscaled if needed.
     However if the render scale is not supported but input images are not rendered at full-scale  ,
     we don't want to cache the full-scale image because it will be low res. Instead in that case we cache the downscaled image
     */
    bool useImageAsOutput;
    RectI roi;
    
    if (renderFullScaleThenDownscale && renderScaleOneUpstreamIfRenderScaleSupportDisabled) {
        
        //We cache 'image', hence the RoI should be expressed in its coordinates
        //renderRoIInternal should check the bitmap of 'image' and not downscaledImage!
        RectD canonicalRoI;
        args.roi.toCanonical(args.mipMapLevel, par, rod, &canonicalRoI);
        canonicalRoI.toPixelEnclosing(0, par, &roi);
        useImageAsOutput = true;
    } else {
        
        //In that case the plug-in either supports render scale or doesn't support render scale but uses downscaled inputs
        //renderRoIInternal should check the bitmap of downscaledImage and not 'image'!
        roi = args.roi;
        useImageAsOutput = false;
    }
    
    
    
    bool tilesSupported = supportsTiles();
    
    
    RectI downscaledImageBounds,upscaledImageBounds;
    rod.toPixelEnclosing(args.mipMapLevel, par, &downscaledImageBounds);
    rod.toPixelEnclosing(0, par, &upscaledImageBounds);
    
    
    
    ///Make sure the RoI falls within the image bounds
    ///Intersection will be in pixel coordinates
    if (tilesSupported) {
        if (useImageAsOutput) {
            if (!roi.intersect(upscaledImageBounds, &roi)) {
                return eRenderRoIRetCodeOk;
            }
            assert(roi.x1 >= upscaledImageBounds.x1 && roi.y1 >= upscaledImageBounds.y1 &&
                   roi.x2 <= upscaledImageBounds.x2 && roi.y2 <= upscaledImageBounds.y2);
            
        } else {
            if (!roi.intersect(downscaledImageBounds, &roi)) {
                return eRenderRoIRetCodeOk;
            }
            assert(roi.x1 >= downscaledImageBounds.x1 && roi.y1 >= downscaledImageBounds.y1 &&
                   roi.x2 <= downscaledImageBounds.x2 && roi.y2 <= downscaledImageBounds.y2);
        }
#ifndef NATRON_ALWAYS_ALLOCATE_FULL_IMAGE_BOUNDS
        ///just allocate the roi
        upscaledImageBounds.intersect(roi, &upscaledImageBounds);
        downscaledImageBounds.intersect(args.roi, &downscaledImageBounds);
#endif
    } else {
        roi = useImageAsOutput ? upscaledImageBounds : downscaledImageBounds;
    }
        
    RectD canonicalRoI;
    if (useImageAsOutput) {
        roi.toCanonical(0, par, rod, &canonicalRoI);
    } else {
        roi.toCanonical(args.mipMapLevel, par, rod, &canonicalRoI);
    }
   
    
    bool createInCache = shouldCacheOutput();

    bool isFrameVaryingOrAnimated = isFrameVaryingOrAnimated_Recursive();
    Natron::ImageKey key = Natron::Image::makeKey(nodeHash, isFrameVaryingOrAnimated, args.time, args.view);

    bool useDiskCacheNode = dynamic_cast<DiskCacheNode*>(this) != NULL;

    {
        ///If the last rendered image had a different hash key (i.e a parameter changed or an input changed)
        ///just remove the old image from the cache to recycle memory.
        ///We also do this if the mipmap level is different (e.g: the user is zooming in/out) because
        ///anyway the ViewerCache will have the texture cached and it would be redundant to keep this image
        ///in the cache since the ViewerCache already has it ready.
        ImageList lastRenderedPlanes;
        U64 lastRenderHash;
        {
            QMutexLocker l(&_imp->lastRenderArgsMutex);
            lastRenderedPlanes = _imp->lastPlanesRendered;
            lastRenderHash = _imp->lastRenderHash;
        }
        if ( !lastRenderedPlanes.empty() && lastRenderHash != nodeHash ) {
            ///once we got it remove it from the cache
            if (!useDiskCacheNode) {
                appPTR->removeAllImagesFromCacheWithMatchingKey(lastRenderHash);
            } else {
                appPTR->removeAllImagesFromDiskCacheWithMatchingKey(lastRenderHash);
            }
            {
                QMutexLocker l(&_imp->lastRenderArgsMutex);
                _imp->lastPlanesRendered.clear();
            }
        }
    }

    
    
    Natron::ImageBitDepthEnum outputDepth;
    std::list<Natron::ImageComponents> outputComponents;
    getPreferredDepthAndComponents(-1, &outputComponents, &outputDepth);
    assert(!outputComponents.empty());
    
    ImagePlanesToRender planesToRender;
    FramesNeededMap framesNeeded;
    
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// Look-up the cache ///////////////////////////////////////////////////////////////
    
    {
        //If one plane is missing from the cache, remove all other planes from the cache
        
        bool attemptToLookupCache = true;
        
        for (std::list<ImageComponents>::iterator it = requestedComponents.begin(); it != requestedComponents.end(); ++it) {
            
            PlaneToRender plane;
            
            /*
             * If the plane is the color plane, we might have to convert between components, hence we always
             * try to find in the cache the "preferred" components of this node for the color plane.
             * For all other planes, just consider this set of components, we do not allow conversion.
             */
            const ImageComponents* components = 0;
            if (!it->isColorPlane()) {
                components = &(*it);
            } else {
                for (std::list<Natron::ImageComponents>::iterator it = outputComponents.begin(); it!=outputComponents.end(); ++it) {
                    if (it->isColorPlane()) {
                        components = &(*it);
                        break;
                    }
                }
            }
            
            getImageFromCacheAndConvertIfNeeded(createInCache, useDiskCacheNode, key, renderMappedMipMapLevel,
                                                useImageAsOutput ? upscaledImageBounds : downscaledImageBounds,
                                                rod,
                                                args.bitdepth, *it,
                                                outputDepth, *components,args.inputImagesList, &plane.fullscaleImage);
            
            
            if (byPassCache) {
                if (plane.fullscaleImage) {
                    appPTR->removeFromNodeCache(key.getHash());
                    plane.fullscaleImage.reset();
                }
                //For writers, we always want to call the render action, but we still want to use the cache for nodes upstream
                if (isWriter()) {
                    byPassCache = false;
                }
            }
            if (plane.fullscaleImage) {
                
                if (!attemptToLookupCache) {
                    appPTR->removeFromNodeCache(plane.fullscaleImage);
                    plane.fullscaleImage.reset();
                } else {
                    
                    //Overwrite the RoD with the RoD contained in the image.
                    //This is to deal with the situation with an image rendered at scale 1 in the cache, but a new render asking for the same
                    //image at scale 0.5. The RoD will then be slightly larger at scale 0.5 thus re-rendering a few pixels. If the effect
                    //wouldn't support tiles, then it'b problematic as it would need to render the whole frame again just for a few pixels.
                    if (!tilesSupported) {
                        rod = plane.fullscaleImage->getRoD();
                    }
                    framesNeeded = plane.fullscaleImage->getParams()->getFramesNeeded();
                }
            } else {
                if (attemptToLookupCache) {
                    attemptToLookupCache = false;
                    //Clear all previous planes
                    for (std::map<ImageComponents, PlaneToRender>::iterator it2 = planesToRender.planes.begin();
                         it2 != planesToRender.planes.end(); ++it2) {
                        if (it2->second.fullscaleImage) {
                            appPTR->removeFromNodeCache(it2->second.fullscaleImage);
                        }
                        it2->second.fullscaleImage.reset();
                        it2->second.downscaleImage.reset();
                    }
                }
            }
            
            plane.downscaleImage = plane.fullscaleImage;
            plane.isAllocatedOnTheFly = false;
            planesToRender.planes.insert(std::make_pair(*it, plane));
        }
    }
    
    assert(!planesToRender.planes.empty());
    
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /////////////////////////////////End cache lookup//////////////////////////////////////////////////////////
    
    if (framesNeeded.empty()) {
        framesNeeded = getFramesNeeded_public(args.time, args.view);
    }
  
    
    ///In the event where we had the image from the cache, but it wasn't completly rendered over the RoI but the cache was almost full,
    ///we don't hold a pointer to it, allowing the cache to free it.
    ///Hence after rendering all the input images, we redo a cache look-up to check whether the image is still here
    bool redoCacheLookup = false;
    bool cacheAlmostFull = appPTR->isNodeCacheAlmostFull();
    
    
    ImagePtr isPlaneCached;
    
    if (!planesToRender.planes.empty()) {
        isPlaneCached = planesToRender.planes.begin()->second.fullscaleImage;
    }
    
    if (!isPlaneCached && args.roi.isNull()) {
        ///Empty RoI and nothing in the cache with matching args, return empty planes.
        return eRenderRoIRetCodeFailed;
    }
    
    if (isPlaneCached) {
        ///We check what is left to render.
#if NATRON_ENABLE_TRIMAP
        if (!frameRenderArgs.canAbort && frameRenderArgs.isRenderResponseToUserInteraction) {
            isPlaneCached->getRestToRender_trimap(roi, planesToRender.rectsToRender, &planesToRender.isBeingRenderedElsewhere);
        } else {
            isPlaneCached->getRestToRender(roi, planesToRender.rectsToRender);
        }
#else
        isPlaneCached->getRestToRender(roi, planesToRender.rectsToRender);
#endif
        
        if (!planesToRender.rectsToRender.empty() && cacheAlmostFull) {
            ///The node cache is almost full and we need to render  something in the image, if we hold a pointer to this image here
            ///we might recursively end-up in this same situation at each level of the render tree, ending with all images of each level
            ///being held in memory.
            ///Our strategy here is to clear the pointer, hence allowing the cache to remove the image, and ask the inputs to render the full RoI
            ///instead of the rest to render. This way, even if the image is cleared from the cache we already have rendered the full RoI anyway.
            planesToRender.rectsToRender.clear();
            planesToRender.rectsToRender.push_back(roi);
            for (std::map<ImageComponents, PlaneToRender>::iterator it2 = planesToRender.planes.begin(); it2 != planesToRender.planes.end(); ++it2) {
                //Keep track of the original cached image for the re-lookup afterward, if the pointer doesn't match the first look-up, don't consider
                //the image because the region to render might have changed and we might have to re-trigger a render on inputs again.
                
                ///Make sure to never dereference originalCachedImage! We only compare it (that's why it s a void*)
                it2->second.originalCachedImage = it2->second.fullscaleImage.get();
                it2->second.fullscaleImage.reset();
                it2->second.downscaleImage.reset();
            }
            isPlaneCached.reset();
            redoCacheLookup = true;
        }
        
        
        
        ///If the effect doesn't support tiles and it has something left to render, just render the bounds again
        ///Note that it should NEVER happen because if it doesn't support tiles in the first place, it would
        ///have rendered the rod already.
        if (!tilesSupported && !planesToRender.rectsToRender.empty() && isPlaneCached) {
            ///if the effect doesn't support tiles, just render the whole rod again even though
            planesToRender.rectsToRender.clear();
            planesToRender.rectsToRender.push_back(isPlaneCached->getBounds());
        }
    } else {
        if (tilesSupported) {
            planesToRender.rectsToRender.push_back(roi);
        } else {
            planesToRender.rectsToRender.push_back(useImageAsOutput ? upscaledImageBounds : downscaledImageBounds);
        }
    }

    
    bool hasSomethingToRender = !planesToRender.rectsToRender.empty();
    
    ///For each rect to render a RoIMap
    std::list<RoIMap> inputsRoi;
    
    ///For each rect to render, the input images
    std::list<ImageList> inputImages;


    ///Pre-render input images before allocating the image if we need to render

    for (std::list<RectI>::iterator it = planesToRender.rectsToRender.begin(); it != planesToRender.rectsToRender.end(); ++it) {
        
        
        RectD canonicalRoI;
        if (useImageAsOutput) {
            it->toCanonical(0, par, rod, &canonicalRoI);
        } else {
            it->toCanonical(args.mipMapLevel, par, rod, &canonicalRoI);
        }

        RoIMap roim;
        ImageList imgs;
        RenderRoIRetCode inputCode = renderInputImagesForRoI(createInCache,
                                                             args.time,
                                                             args.view,
                                                             par,
                                                             nodeHash,
                                                             frameRenderArgs.rotoAge,
                                                             rod,
                                                             *it,
                                                             canonicalRoI,
                                                             inputsToTransform,
                                                             args.mipMapLevel,
                                                             args.scale,
                                                             renderMappedScale,
                                                             renderScaleOneUpstreamIfRenderScaleSupportDisabled,
                                                             byPassCache,
                                                             framesNeeded,
                                                             &imgs,
                                                             &roim);
        //Render was aborted
        if (inputCode != eRenderRoIRetCodeOk) {
            return inputCode;
        }
        inputsRoi.push_back(roim);
        inputImages.push_back(imgs);
    }
    
    if (redoCacheLookup) {
        
        for (std::map<ImageComponents, PlaneToRender>::iterator it = planesToRender.planes.begin(); it != planesToRender.planes.end(); ++it) {
            
            /*
             * If the plane is the color plane, we might have to convert between components, hence we always
             * try to find in the cache the "preferred" components of this node for the color plane.
             * For all other planes, just consider this set of components, we do not allow conversion.
             */
            const ImageComponents* components = 0;
            if (!it->first.isColorPlane()) {
                components = &(it->first);
            } else {
                for (std::list<Natron::ImageComponents>::iterator it = outputComponents.begin(); it!=outputComponents.end(); ++it) {
                    if (it->isColorPlane()) {
                        components = &(*it);
                        break;
                    }
                }
            }

            assert(components);
            getImageFromCacheAndConvertIfNeeded(createInCache, useDiskCacheNode, key, renderMappedMipMapLevel,
                                                useImageAsOutput ? upscaledImageBounds : downscaledImageBounds,
                                                rod,
                                                args.bitdepth, it->first,
                                                outputDepth,*components,
                                                args.inputImagesList, &it->second.fullscaleImage);
            
            ///We must retrieve from the cache exactly the originally retrieved image, otherwise we might have to call  renderInputImagesForRoI
            ///again, which could create a vicious cycle.
            if (it->second.fullscaleImage && it->second.fullscaleImage.get() == it->second.originalCachedImage) {
                it->second.downscaleImage = it->second.fullscaleImage;
            } else {
                for (std::map<ImageComponents, PlaneToRender>::iterator it2 = planesToRender.planes.begin(); it2 != planesToRender.planes.end(); ++it2) {
                    it2->second.fullscaleImage.reset();
                    it2->second.downscaleImage.reset();
                }
                break;
            }
            
            
        }
        
        isPlaneCached = planesToRender.planes.begin()->second.fullscaleImage;
        
        if (!isPlaneCached) {
            planesToRender.rectsToRender.clear();
            if (tilesSupported) {
                planesToRender.rectsToRender.push_back(roi);
            } else {
                planesToRender.rectsToRender.push_back(useImageAsOutput ? upscaledImageBounds : downscaledImageBounds);
            }
            inputImages.clear();
            inputsRoi.clear();
            
            ///We must re-copute input images because we might not have rendered what's needed
            for (std::list<RectI>::iterator it = planesToRender.rectsToRender.begin(); it != planesToRender.rectsToRender.end(); ++it) {
                
                
                RectD canonicalRoI;
                if (useImageAsOutput) {
                    it->toCanonical(0, par, rod, &canonicalRoI);
                } else {
                    it->toCanonical(args.mipMapLevel, par, rod, &canonicalRoI);
                }
                
                RoIMap roim;
                ImageList imgs;
                RenderRoIRetCode inputRetCode = renderInputImagesForRoI(createInCache,
                                                                        args.time,
                                                                        args.view,
                                                                        par,
                                                                        nodeHash,
                                                                        frameRenderArgs.rotoAge,
                                                                        rod,
                                                                        *it,
                                                                        canonicalRoI,
                                                                        inputsToTransform,
                                                                        args.mipMapLevel,
                                                                        args.scale,
                                                                        renderMappedScale,
                                                                        renderScaleOneUpstreamIfRenderScaleSupportDisabled,
                                                                        byPassCache,
                                                                        framesNeeded,
                                                                        &imgs,
                                                                        &roim);
                    //Render was aborted
                if (inputRetCode != eRenderRoIRetCodeOk) {
                    return inputRetCode;
                }
                inputsRoi.push_back(roim);
                inputImages.push_back(imgs);
            }
        }
        
    } // if (redoCacheLookup) {

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// Allocate planes in the cache ////////////////////////////////////////////////////////////
    
    ///For all planes, if needed allocate the associated image
    for (std::map<ImageComponents, PlaneToRender>::iterator it = planesToRender.planes.begin();
         it != planesToRender.planes.end(); ++it) {
        
        
        const ImageComponents* components;
        if (!it->first.isColorPlane()) {
            components = &(it->first);
        } else {
            for (std::list<Natron::ImageComponents>::iterator it = outputComponents.begin(); it!=outputComponents.end(); ++it) {
                if (it->isColorPlane()) {
                    components = &(*it);
                    break;
                }
            }
        }
        
        if (!it->second.fullscaleImage) {
            ///The image is not cached
            
            allocateImagePlane(key, rod, downscaledImageBounds, upscaledImageBounds, isProjectFormat, framesNeeded, *components, outputDepth, par, args.mipMapLevel, renderFullScaleThenDownscale, renderScaleOneUpstreamIfRenderScaleSupportDisabled, useDiskCacheNode, createInCache, &it->second.fullscaleImage, &it->second.downscaleImage);

        } else {
            if (renderFullScaleThenDownscale && it->second.fullscaleImage->getMipMapLevel() == 0) {
                //Allocate a downscale image that will be cheap to create
                ///The upscaled image will be rendered using input images at lower def... which means really crappy results, don't cache this image!
                RectI bounds;
                rod.toPixelEnclosing(args.mipMapLevel, par, &bounds);
                it->second.downscaleImage.reset( new Natron::Image(*components, rod, downscaledImageBounds, args.mipMapLevel, it->second.fullscaleImage->getPixelAspectRatio(), outputDepth, true) );
                it->second.fullscaleImage->downscaleMipMap(rod,it->second.fullscaleImage->getBounds(), 0, args.mipMapLevel, true, it->second.downscaleImage.get());
            }
        }
        
        ///The image and downscaled image are pointing to the same image in 2 cases:
        ///1) Proxy mode is turned off
        ///2) Proxy mode is turned on but plug-in supports render scale
        ///Subsequently the image and downscaled image are different only if the plug-in
        ///does not support the render scale and the proxy mode is turned on.
        assert( (it->second.fullscaleImage == it->second.downscaleImage && !renderFullScaleThenDownscale) ||
               ((it->second.fullscaleImage != it->second.downscaleImage || it->second.fullscaleImage->getMipMapLevel() == it->second.downscaleImage->getMipMapLevel()) && renderFullScaleThenDownscale) );
    }
    
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// End allocation of planes ///////////////////////////////////////////////////////////////
    
    
    //There should always be at least 1 plane to render (The color plane)
    assert(!planesToRender.planes.empty());

    ///If we reach here, it can be either because the planes are cached or not, either way
    ///the planes are NOT an identity, and they may have some content left to render.
    EffectInstance::RenderRoIStatusEnum renderRetCode = eRenderRoIStatusImageAlreadyRendered;
    
    bool renderAborted;

    if (!hasSomethingToRender && !planesToRender.isBeingRenderedElsewhere) {
        renderAborted = aborted();
    } else {
    
#if NATRON_ENABLE_TRIMAP
        ///Only use trimap system if the render cannot be aborted.
        if (!frameRenderArgs.canAbort && frameRenderArgs.isRenderResponseToUserInteraction) {
            for (std::map<ImageComponents, PlaneToRender>::iterator it = planesToRender.planes.begin(); it != planesToRender.planes.end(); ++it) {
                _imp->markImageAsBeingRendered(useImageAsOutput ? it->second.fullscaleImage : it->second.downscaleImage);
            }
        }
#endif


        if (hasSomethingToRender) {
            
# ifdef DEBUG

            {
                const std::list<RectI>& rectsToRender = planesToRender.rectsToRender;
                
                qDebug() <<'('<<QThread::currentThread()->objectName()<<")--> "<< getNode()->getScriptName_mt_safe().c_str() << ": render view " << args.view << " " << rectsToRender.size() << " rectangles";
                for (std::list<RectI>::const_iterator it = rectsToRender.begin(); it != rectsToRender.end(); ++it) {
                    qDebug() << "rect: " << "x1= " <<  it->x1 << " , y1= " << it->y1 << " , x2= " << it->x2 << " , y2= " << it->y2;
                }
            }
# endif
            renderRetCode = renderRoIInternal(args.time,
                                              args.mipMapLevel,
                                              args.view,
                                              rod,
                                              par,
                                              planesToRender,
                                              useImageAsOutput,
                                              frameRenderArgs.isSequentialRender,
                                              frameRenderArgs.isRenderResponseToUserInteraction,
                                              nodeHash,
                                              args.channelForAlpha,
                                              renderFullScaleThenDownscale,
                                              renderScaleOneUpstreamIfRenderScaleSupportDisabled,
                                              inputsRoi,
                                              inputImages);
        }
        
        renderAborted = aborted();
#if NATRON_ENABLE_TRIMAP
        
        if (!frameRenderArgs.canAbort && frameRenderArgs.isRenderResponseToUserInteraction) {
            ///Only use trimap system if the render cannot be aborted.
            ///If we were aborted after all (because the node got deleted) then return a NULL image and empty the cache
            ///of this image
            
            for (std::map<ImageComponents, PlaneToRender>::iterator it = planesToRender.planes.begin(); it!=planesToRender.planes.end(); ++it) {
                if (!renderAborted) {
                    if (renderRetCode == eRenderRoIStatusRenderFailed || !planesToRender.isBeingRenderedElsewhere) {
                        _imp->unmarkImageAsBeingRendered(useImageAsOutput ? it->second.fullscaleImage : it->second.downscaleImage,
                                                         renderRetCode == eRenderRoIStatusRenderFailed);
                    } else {
                        _imp->waitForImageBeingRenderedElsewhereAndUnmark(roi,
                                                                          useImageAsOutput ? it->second.fullscaleImage: it->second.downscaleImage);
                    }
                } else {
                    _imp->unmarkImageAsBeingRendered(useImageAsOutput ? it->second.fullscaleImage : it->second.downscaleImage,true);
                    appPTR->removeFromNodeCache(useImageAsOutput ? it->second.fullscaleImage : it->second.downscaleImage);
                    return eRenderRoIRetCodeAborted;
                }
            }
        }
#endif
    }
    
    
    if ( renderAborted && renderRetCode != eRenderRoIStatusImageAlreadyRendered) {
        
        ///Return a NULL image if the render call was not issued by the result of a call of a plug-in to clipGetImage
        //if (!args.calledFromGetImage) {
        return eRenderRoIRetCodeAborted;
        //}
        
    } else if (renderRetCode == eRenderRoIStatusRenderFailed) {
        throw std::runtime_error("Rendering Failed");
    }
    
    
#ifdef DEBUG
    if (renderRetCode != eRenderRoIStatusRenderFailed && !renderAborted) {
        // Kindly check that everything we asked for is rendered!
        
        for (std::map<ImageComponents, PlaneToRender>::iterator it = planesToRender.planes.begin(); it!=planesToRender.planes.end(); ++it) {
            std::list<RectI> restToRender;
            if (useImageAsOutput) {
                it->second.fullscaleImage->getRestToRender(roi,restToRender);
            } else {
                it->second.downscaleImage->getRestToRender(roi,restToRender);
            }
            assert(restToRender.empty());
        }
    }
#endif
    
    
    for (std::map<ImageComponents, PlaneToRender>::iterator it = planesToRender.planes.begin(); it != planesToRender.planes.end(); ++it) {
        //We have to return the downscale image, so make sure it has been computed
        if (renderRetCode != eRenderRoIStatusRenderFailed && renderFullScaleThenDownscale && renderScaleOneUpstreamIfRenderScaleSupportDisabled) {
            assert(it->second.fullscaleImage->getMipMapLevel() == 0);
            roi.intersect(it->second.fullscaleImage->getBounds(), &roi);
            it->second.fullscaleImage->downscaleMipMap(it->second.fullscaleImage->getRoD(),roi, 0, args.mipMapLevel, false, it->second.downscaleImage.get());
        }

        ///The image might need to be converted to fit the original requested format
        bool imageConversionNeeded = it->first != it->second.downscaleImage->getComponents() || args.bitdepth != it->second.downscaleImage->getBitDepth();
        
        if (imageConversionNeeded && renderRetCode != eRenderRoIStatusRenderFailed) {
            
            /**
             * Lock the downscaled image so it cannot be resized while creating the temp image and calling convertToFormat.
             **/
            boost::shared_ptr<Image> tmp;
            {
                Image::ReadAccess acc = it->second.downscaleImage->getReadRights();
                
                tmp.reset( new Image(it->first, it->second.downscaleImage->getRoD(), it->second.downscaleImage->getBounds(), mipMapLevel,it->second.downscaleImage->getPixelAspectRatio(), args.bitdepth, false) );
                
                bool unPremultIfNeeded = getOutputPremultiplication() == eImagePremultiplicationPremultiplied;
                it->second.downscaleImage->convertToFormat(it->second.downscaleImage->getBounds(),
                                                           getApp()->getDefaultColorSpaceForBitDepth(it->second.downscaleImage->getBitDepth()),
                                                           getApp()->getDefaultColorSpaceForBitDepth(args.bitdepth),
                                                           args.channelForAlpha, false, false, unPremultIfNeeded, tmp.get());
            }
            it->second.downscaleImage = tmp;
        }
        
        assert(it->second.downscaleImage->getComponents() == it->first && it->second.downscaleImage->getBitDepth() == args.bitdepth);
        outputPlanes->push_back(it->second.downscaleImage);

    }
    
    {
        ///flag that this is the last image we rendered
        QMutexLocker l(&_imp->lastRenderArgsMutex);
        _imp->lastRenderHash = nodeHash;
        _imp->lastPlanesRendered = *outputPlanes;
    }
    return eRenderRoIRetCodeOk;
} // renderRoI


EffectInstance::RenderRoIRetCode
EffectInstance::renderInputImagesForRoI(bool createImageInCache,
                                        SequenceTime time,
                                        int view,
                                        double par,
                                        U64 nodeHash,
                                        U64 rotoAge,
                                        const RectD& rod,
                                        const RectI& downscaledRenderWindow,
                                        const RectD& canonicalRenderWindow,
                                        const std::list<InputMatrix>& inputTransforms,
                                        unsigned int mipMapLevel,
                                        const RenderScale & scale,
                                        const RenderScale& renderMappedScale,
                                        bool useScaleOneInputImages,
                                        bool byPassCache,
                                        const FramesNeededMap& framesNeeded,
                                        ImageList *inputImages,
                                        RoIMap* inputsRoi)
{
    getRegionsOfInterest_public(time, renderMappedScale, rod, canonicalRenderWindow, view,inputsRoi);
#ifdef DEBUG
    if (!inputsRoi->empty() && framesNeeded.empty() && !isReader()) {
        qDebug() << getNode()->getScriptName_mt_safe().c_str() << ": getRegionsOfInterestAction returned 1 or multiple input RoI(s) but returned "
        << "an empty list with getFramesNeededAction";
    }
#endif
    
    std::map<int, EffectInstance*> reroutesMap;
    //Transform the RoIs by the inverse of the transform matrix (which is in pixel coordinates)
    for (std::list<InputMatrix>::const_iterator it = inputTransforms.begin(); it != inputTransforms.end(); ++it) {
        
        RectD transformedRenderWindow;
        
        Natron::EffectInstance* effectInTransformInput = getInput(it->inputNb);
        assert(effectInTransformInput);
        
        
        
        RoIMap::iterator foundRoI = inputsRoi->find(effectInTransformInput);
        if (foundRoI == inputsRoi->end()) {
            //There might be no RoI because it was null 
            continue;
        }
        
        // invert it
        Transform::Matrix3x3 invertTransform;
        double det = Transform::matDeterminant(*it->cat);
        if (det != 0.) {
            invertTransform = Transform::matInverse(*it->cat, det);
        }
        
        Transform::Matrix3x3 canonicalToPixel = Transform::matCanonicalToPixel(par, scale.x,
                                                                               scale.y, false);
        Transform::Matrix3x3 pixelToCanonical = Transform::matPixelToCanonical(par,  scale.x,
                                                                               scale.y, false);
        
        invertTransform = Transform::matMul(Transform::matMul(pixelToCanonical, invertTransform), canonicalToPixel);
        Transform::transformRegionFromRoD(foundRoI->second, invertTransform, transformedRenderWindow);
        
        //Replace the original RoI by the transformed RoI
        inputsRoi->erase(foundRoI);
        inputsRoi->insert(std::make_pair(it->newInputEffect->getInput(it->newInputNbToFetchFrom), transformedRenderWindow));
        reroutesMap.insert(std::make_pair(it->inputNb, it->newInputEffect));

    }

    ComponentsNeededMap neededComps;
    SequenceTime ptTime;
    int ptView;
    boost::shared_ptr<Natron::Node> ptInput;
    getComponentsNeededAndProduced_public(time, view, &neededComps, &ptTime, &ptView, &ptInput);

    
    for (FramesNeededMap::const_iterator it = framesNeeded.begin(); it != framesNeeded.end(); ++it) {
        ///We have to do this here because the enabledness of a mask is a feature added by Natron.
        bool inputIsMask = isInputMask(it->first);
        if ( inputIsMask && !isMaskEnabled(it->first) ) {
            continue;
        }
        
        ///There cannot be frames needed without components needed.
        ComponentsNeededMap::iterator foundCompsNeeded = neededComps.find(it->first);
        if (foundCompsNeeded == neededComps.end()) {
            continue;
        }
        
        EffectInstance* inputEffect;
        std::map<int, EffectInstance*>::iterator foundReroute = reroutesMap.find(it->first);
        if (foundReroute != reroutesMap.end()) {
            inputEffect = foundReroute->second->getInput(it->first);
        } else {
            inputEffect = getInput(it->first);
        }

        if (inputEffect) {
            
            ///What region are we interested in for this input effect ? (This is in Canonical coords)
            RoIMap::iterator foundInputRoI = inputsRoi->find(inputEffect);
            if(foundInputRoI == inputsRoi->end()) {
                continue;
            }
            
            ///Convert to pixel coords the RoI
            if ( foundInputRoI->second.isInfinite() ) {
                throw std::runtime_error(std::string("Plugin ") + this->getPluginLabel() + " asked for an infinite region of interest!");
            }
            
            const double inputPar = inputEffect->getPreferredAspectRatio();
            
            RectI inputRoIPixelCoords;
            foundInputRoI->second.toPixelEnclosing(useScaleOneInputImages ? 0 : mipMapLevel, inputPar, &inputRoIPixelCoords);
            
            ///Notify the node that we're going to render something with the input
            assert(it->first != -1); //< see getInputNumber
            
            {
                NotifyInputNRenderingStarted_RAII inputNIsRendering_RAII(getNode().get(),it->first);
                
                ///For all frames requested for this node, render the RoI requested.
                
                for (std::map<int, std::vector<OfxRangeD> >::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
                    
                    for (U32 range = 0; range < it2->second.size(); ++range) {
                        for (int f = std::floor(it2->second[range].min + 0.5); f <= std::floor(it2->second[range].max + 0.5); ++f) {
                            
                            
                            int channelForAlphaInput = inputIsMask ? getMaskChannel(it->first) : 3;
                            RenderScale scaleOne;
                            scaleOne.x = scaleOne.y = 1.;
                            
                            ///Render the input image with the bit depth of its preference
                            std::list<Natron::ImageComponents> inputPrefComps;
                            Natron::ImageBitDepthEnum inputPrefDepth;
                            inputEffect->getPreferredDepthAndComponents(-1/*it2->first*/, &inputPrefComps, &inputPrefDepth);
                            std::list<ImageComponents> componentsToRender;
                            for (U32 k = 0; k < foundCompsNeeded->second.size(); ++k) {
                                componentsToRender.push_back(foundCompsNeeded->second[k]);
                            }
                            
                            RenderRoIArgs inArgs(f, //< time
                                                 useScaleOneInputImages ? scaleOne : scale, //< scale
                                                 useScaleOneInputImages ? 0 : mipMapLevel, //< mipmapLevel (redundant with the scale)
                                                 view, //< view
                                                 byPassCache,
                                                 inputRoIPixelCoords, //< roi in pixel coordinates
                                                 RectD(), // < did we precompute any RoD to speed-up the call ?
                                                 componentsToRender, //< requested comps
                                                 inputPrefDepth,
                                                 channelForAlphaInput);
                            
                            
                           
                            ImageList inputImgs;
                            RenderRoIRetCode ret = inputEffect->renderRoI(inArgs, &inputImgs); //< requested bitdepth
                            if (ret != eRenderRoIRetCodeOk) {
                                return ret;
                            }
                            
                            for (ImageList::iterator it3 = inputImgs.begin(); it3 != inputImgs.end(); ++it3) {
                                if (*it3) {
                                    inputImages->push_back(*it3);
                                }
                            }
                        }
                    }
                }
    
                
            } // NotifyInputNRenderingStarted_RAII inputNIsRendering_RAII(_node.get(),it2->first);
            
            if ( aborted() ) {
                return eRenderRoIRetCodeAborted;
            }
        }
    }

    
    ///if the node has a roto context, pre-render the roto mask too
    boost::shared_ptr<RotoContext> rotoCtx = getNode()->getRotoContext();
    if (rotoCtx) {
        std::list<Natron::ImageComponents> inputPrefComps;
        Natron::ImageBitDepthEnum inputPrefDepth;
        int rotoIndex = getRotoBrushInputIndex();
        assert(rotoIndex != -1);
        getPreferredDepthAndComponents(rotoIndex, &inputPrefComps, &inputPrefDepth);
        
        //Roto can only output color
        assert(!inputPrefComps.empty() && inputPrefComps.front().isColorPlane());
        
        boost::shared_ptr<Natron::Image> mask = rotoCtx->renderMask(createImageInCache,
                                                                    downscaledRenderWindow,
                                                                    inputPrefComps.front(),
                                                                    nodeHash,
                                                                    rotoAge,
                                                                    rod,
                                                                    time,
                                                                    inputPrefDepth,
                                                                    view,
                                                                    mipMapLevel,
                                                                    ImageList(),
                                                                    byPassCache);
        assert(mask);
        inputImages->push_back(mask);
    }
    
    return eRenderRoIRetCodeOk;
}


EffectInstance::RenderRoIStatusEnum
EffectInstance::renderRoIInternal(SequenceTime time,
                                  unsigned int mipMapLevel,
                                  int view,
                                  const RectD & rod, //!< effect rod in canonical coords
                                  const double par,
                                  ImagePlanesToRender& planesToRender,
                                  bool outputUseImage, //< whether we output to image or downscaledImage
                                  bool isSequentialRender,
                                  bool isRenderMadeInResponseToUserInteraction,
                                  U64 nodeHash,
                                  int channelForAlpha,
                                  bool renderFullScaleThenDownscale,
                                  bool useScaleOneInputImages,
                                  const std::list<RoIMap>& inputsRoi,
                                  std::list<std::list<boost::shared_ptr<Natron::Image> > >& inputImages)
{
    EffectInstance::RenderRoIStatusEnum retCode;
    
    assert(!planesToRender.planes.empty());
    
    ///Add the window to the project's available formats if the effect is a reader
    ///This is the only reliable place where I could put these lines...which don't seem to feel right here.
    ///Plus setOrAddProjectFormat will actually set the project format the first time we read an image in the project
    ///hence ask for a new render... which can be expensive!
    ///Any solution how to work around this ?
    if ( isReader() ) {
        Format frmt;
        RectI pixelRoD;
        rod.toPixelEnclosing(0, par, &pixelRoD);
        frmt.set(pixelRoD);
        frmt.setPixelAspectRatio(par);
        getApp()->getProject()->setOrAddProjectFormat(frmt);
    }
    
    RenderScale renderMappedScale;
    

    for (std::map<ImageComponents,PlaneToRender>::iterator it = planesToRender.planes.begin(); it!=planesToRender.planes.end(); ++it) {
        it->second.renderMappedImage = renderFullScaleThenDownscale ? it->second.fullscaleImage : it->second.downscaleImage;
        if (it == planesToRender.planes.begin()) {
            renderMappedScale.x = Image::getScaleFromMipMapLevel(it->second.renderMappedImage->getMipMapLevel());
            renderMappedScale.y = renderMappedScale.x;
        }
    }
    
    const PlaneToRender& firstPlaneToRender = planesToRender.planes.begin()->second;

    bool tilesSupported = supportsTiles();


    Natron::StatusEnum renderStatus = eStatusOK;
    if (planesToRender.rectsToRender.empty()) {
        retCode = EffectInstance::eRenderRoIStatusImageAlreadyRendered;
    } else {
        retCode = EffectInstance::eRenderRoIStatusImageRendered;
    }


    ///Notify the gui we're rendering
    boost::shared_ptr<NotifyRenderingStarted_RAII> renderingNotifier;
    if (!planesToRender.rectsToRender.empty()) {
        renderingNotifier.reset(new NotifyRenderingStarted_RAII(getNode().get()));
    }

    /*depending on the thread-safety of the plug-in we render with a different
     amount of threads*/
    EffectInstance::RenderSafetyEnum safety = renderThreadSafety();
    
    ///if the project lock is already locked at this point, don't start any other thread
    ///as it would lead to a deadlock when the project is loading.
    ///Just fall back to Fully_safe
    int nbThreads = appPTR->getCurrentSettings()->getNumberOfThreads();
    if (safety == eRenderSafetyFullySafeFrame) {
        ///If the plug-in is eRenderSafetyFullySafeFrame that means it wants the host to perform SMP aka slice up the RoI into chunks
        ///but if the effect doesn't support tiles it won't work.
        ///Also check that the number of threads indicating by the settings are appropriate for this render mode.
        if ( !tilesSupported || (nbThreads == -1) || (nbThreads == 1) ||
            ( (nbThreads == 0) && (appPTR->getHardwareIdealThreadCount() == 1) ) ||
            ( QThreadPool::globalInstance()->activeThreadCount() >= QThreadPool::globalInstance()->maxThreadCount() ) ) {
            safety = eRenderSafetyFullySafe;
        } else {
            if ( !getApp()->getProject()->tryLock() ) {
                safety = eRenderSafetyFullySafe;
            } else {
                getApp()->getProject()->unlock();
            }
        }
    }
    
    std::map<boost::shared_ptr<Natron::Node>,ParallelRenderArgs > tlsCopy;
    if (safety == eRenderSafetyFullySafeFrame) {
        /*
         * Since we're about to start new threads potentially, copy all the thread local storage on all nodes (any node may be involved in
         * expressions, and we need to retrieve the exact local time of render).
         */
        getApp()->getProject()->getParallelRenderArgs(tlsCopy);

    }
    
    assert(inputsRoi.size() == planesToRender.rectsToRender.size() && inputImages.size() == planesToRender.rectsToRender.size());
    
    std::list<RoIMap>::const_iterator roiIT = inputsRoi.begin();
    std::list<ImageList>::const_iterator inputImgIt = inputImages.begin();
    for (std::list<RectI>::const_iterator it = planesToRender.rectsToRender.begin(); it != planesToRender.rectsToRender.end(); ++it,++roiIT,++inputImgIt) {
        
        
        assert(!it->isNull());
        
        ///We hold our input images in thread-storage, so that the getImage function can find them afterwards, even if the node doesn't cache its output.
        boost::shared_ptr<InputImagesHolder_RAII> inputImagesHolder;
        if (!inputImages.empty()) {
            inputImagesHolder.reset(new InputImagesHolder_RAII(*inputImgIt,&_imp->inputImages));
        }
        
        RectI downscaledRectToRender = *it; // please leave it as const, copy it if necessary

        ///Upscale the RoI to a region in the full scale image so it is in canonical coordinates
        RectD canonicalRectToRender;
        downscaledRectToRender.toCanonical(outputUseImage ? firstPlaneToRender.fullscaleImage->getMipMapLevel() : firstPlaneToRender.downscaleImage->getMipMapLevel(), par, rod, &canonicalRectToRender);
        
        if (outputUseImage && renderFullScaleThenDownscale && mipMapLevel > 0) {
            downscaledRectToRender = downscaledRectToRender.downscalePowerOfTwoSmallestEnclosing(mipMapLevel);
        }

        ///the getRegionsOfInterest call will not be cached because it would be unnecessary
        ///To put that information (which depends on the RoI) into the cache. That's why we
        ///store it into the render args (thread-storage) so the getImage() function can retrieve the results.
        assert( !( (supportsRenderScaleMaybe() == eSupportsNo) && !(renderMappedScale.x == 1. && renderMappedScale.y == 1.) ) );

        
        ///There cannot be the same thread running 2 concurrent instances of renderRoI on the same effect.
        assert(!_imp->renderArgs.hasLocalData() || !_imp->renderArgs.localData()._validArgs);

        RectI renderMappedRectToRender;
        
        if (renderFullScaleThenDownscale) {
            canonicalRectToRender.toPixelEnclosing(0, par, &renderMappedRectToRender);
            renderMappedRectToRender.intersect(firstPlaneToRender.renderMappedImage->getBounds(), &renderMappedRectToRender);
        } else {
            renderMappedRectToRender = downscaledRectToRender;
        }
        
        Implementation::ScopedRenderArgs scopedArgs(&_imp->renderArgs);
        scopedArgs.setArgs_firstPass(rod,
                                     renderMappedRectToRender,
                                     time,
                                     view,
                                     channelForAlpha,
                                     false, //< if we reached here the node is not an identity!
                                     0.,
                                     -1);
        
        
        int firstFrame, lastFrame;
        getFrameRange_public(nodeHash, &firstFrame, &lastFrame);
        
        ///The scoped args will maintain the args set for this thread during the
        ///whole time the render action is called, so they can be fetched in the
        ///getImage() call.
        /// @see EffectInstance::getImage
        scopedArgs.setArgs_secondPass(*roiIT,firstFrame,lastFrame);
        RenderArgs & args = scopedArgs.getLocalData();


#ifndef NDEBUG
        RenderScale scale;
        scale.x = Image::getScaleFromMipMapLevel(mipMapLevel);
        scale.y = scale.x;
        // check the dimensions of all input and output images
        for (std::list< boost::shared_ptr<Natron::Image> >::const_iterator it = inputImgIt->begin();
             it != inputImgIt->end();
             ++it) {

            assert(useScaleOneInputImages || (*it)->getMipMapLevel() == mipMapLevel);
            const RectD & srcRodCanonical = (*it)->getRoD();
            RectI srcBounds;
            srcRodCanonical.toPixelEnclosing((*it)->getMipMapLevel(), (*it)->getPixelAspectRatio(), &srcBounds); // compute srcRod at level 0
            const RectD & dstRodCanonical = firstPlaneToRender.renderMappedImage->getRoD();
            RectI dstBounds;
            dstRodCanonical.toPixelEnclosing(firstPlaneToRender.renderMappedImage->getMipMapLevel(), par, &dstBounds); // compute dstRod at level 0

            if (!tilesSupported) {
                // http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#kOfxImageEffectPropSupportsTiles
                //  If a clip or plugin does not support tiled images, then the host should supply full RoD images to the effect whenever it fetches one.

                ///Note: The renderRoI() function returns an image according to the mipMapLevel given in parameters.
                ///For effects that DO NOT SUPPORT TILES they are expected an input image to be the full RoD.
                ///Hence the resulting image of the renderRoI call made on the input has to be upscaled to its full RoD.
                ///The reason why this upscale is done externally to renderRoI is because renderRoI is "local" to an effect:
                ///The effect has no way to know that the caller (downstream effect) doesn't support tiles. We would have to
                ///pass this in parameters to the renderRoI function and would make it less clear to the caller.
                ///
                ///Another point is that we don't cache the resulting upscaled image (@see getImage()).
                ///The reason why we don't do this is because all images in the NodeCache have a key identifying them.
                ///Part of the key is the mipmapLevel of the image, hence
                ///2 images with different mipmapLevels have different keys. Now if we were to put those "upscaled" images in the cache
                ///they would take the same priority as the images that were REALLY rendered at scale 1. But those upcaled images have poor
                ///quality compared to the images rendered at scale 1, hence we don't cache them.
                ///If we were to cache them, we would need to change the way the cache works and return a list of potential images instead.
                ///This way we could add a "quality" identifier to images and pick the best one from the list returned by the cache.
                RectI srcRealBounds = (*it)->getBounds();
                RectI dstRealBounds = firstPlaneToRender.renderMappedImage->getBounds();
                
                assert(srcRealBounds.x1 == srcBounds.x1);
                assert(srcRealBounds.x2 == srcBounds.x2);
                assert(srcRealBounds.y1 == srcBounds.y1);
                assert(srcRealBounds.y2 == srcBounds.y2);
                assert(dstRealBounds.x1 == dstBounds.x1);
                assert(dstRealBounds.x2 == dstBounds.x2);
                assert(dstRealBounds.y1 == dstBounds.y1);
                assert(dstRealBounds.y2 == dstBounds.y2);
            }
            if ( !supportsMultiResolution() ) {
                // http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#kOfxImageEffectPropSupportsMultiResolution
                //   Multiple resolution images mean...
                //    input and output images can be of any size
                //    input and output images can be offset from the origin
                assert(srcBounds.x1 == 0);
                assert(srcBounds.y1 == 0);
                assert(srcBounds.x1 == dstBounds.x1);
                assert(srcBounds.x2 == dstBounds.x2);
                assert(srcBounds.y1 == dstBounds.y1);
                assert(srcBounds.y2 == dstBounds.y2);
            }
        } //end for
        
        if (supportsRenderScaleMaybe() == eSupportsNo) {
            assert(firstPlaneToRender.renderMappedImage->getMipMapLevel() == 0);
            assert(renderMappedScale.x == 1. && renderMappedScale.y == 1.);
        }
#     endif // DEBUG

       

        ///We only need to call begin if we've not already called it.
        bool callBegin = false;

        /// call beginsequenceRender here if the render is sequential
        
        Natron::SequentialPreferenceEnum pref = getSequentialPreference();
        if (!isWriter() || pref == eSequentialPreferenceNotSequential) {
            callBegin = true;
        }
        
        
        // eRenderSafetyInstanceSafe means that there is at most one render per instance
        // NOTE: the per-instance lock should probably be shared between
        // all clones of the same instance, because an InstanceSafe plugin may assume it is the sole owner of the output image,
        // and read-write on it.
        // It is probably safer to assume that several clones may write to the same output image only in the eRenderSafetyFullySafe case.
        
        // eRenderSafetyFullySafe means that there is only one render per FRAME : the lock is by image and handled in Node.cpp
        ///locks belongs to an instance)
        
        boost::shared_ptr<QMutexLocker> locker;
        
        if (safety == eRenderSafetyInstanceSafe) {
            locker.reset(new QMutexLocker( &getNode()->getRenderInstancesSharedMutex()));
        } else if (safety == eRenderSafetyUnsafe) {
            const Natron::Plugin* p = getNode()->getPlugin();
            assert(p);
            locker.reset(new QMutexLocker(p->getPluginLock()));
        }
        ///For eRenderSafetyFullySafe, don't take any lock, the image already has a lock on itself so we're sure it can't be written to by 2 different threads.


        if (callBegin) {
            assert( !( (supportsRenderScaleMaybe() == eSupportsNo) && !(renderMappedScale.x == 1. && renderMappedScale.y == 1.) ) );
            if (beginSequenceRender_public(time, time, 1, !appPTR->isBackground(), renderMappedScale, isSequentialRender,
                                           isRenderMadeInResponseToUserInteraction, view) == eStatusFailed) {
                renderStatus = eStatusFailed;
                break;
            }
        }

        assert(_imp->frameRenderArgs.hasLocalData());
        const ParallelRenderArgs& frameArgs = _imp->frameRenderArgs.localData();

        switch (safety) {
        case eRenderSafetyFullySafeFrame: {     // the plugin will not perform any per frame SMP threading
            // we can split the frame in tiles and do per frame SMP threading (see kOfxImageEffectPluginPropHostFrameThreading)
            if (nbThreads == 0) {
                nbThreads = QThreadPool::globalInstance()->maxThreadCount();
            }
            std::vector<RectI> splitRects = downscaledRectToRender.splitIntoSmallerRects(nbThreads);
            
            TiledRenderingFunctorArgs tiledArgs;
            tiledArgs.args = &args;
            tiledArgs.isSequentialRender = isSequentialRender;
            tiledArgs.inputImages = *inputImgIt;
            tiledArgs.renderUseScaleOneInputs = useScaleOneInputImages;
            tiledArgs.isRenderResponseToUserInteraction = isRenderMadeInResponseToUserInteraction;
            tiledArgs.planes = &planesToRender;
            tiledArgs.par = par;
            tiledArgs.renderFullScaleThenDownscale = renderFullScaleThenDownscale;
//#define NATRON_HOSTFRAMETHREADING_SEQUENTIAL // sequential execution of host threading
#ifdef NATRON_HOSTFRAMETHREADING_SEQUENTIAL
            std::vector<EffectInstance::RenderingFunctorRetEnum> ret(splitRects.size());
            for (size_t i = 0; i < splitRects.size(); ++i) {
                ret[i] = tiledRenderingFunctor(tiledArgs,
                                               frameArgs,
                                               true,
                                               splitRects[i]);
            }
#else
            // the bitmap is checked again at the beginning of EffectInstance::tiledRenderingFunctor()
            QFuture<EffectInstance::RenderingFunctorRetEnum> ret = QtConcurrent::mapped( splitRects,
                                                                                    boost::bind(&EffectInstance::tiledRenderingFunctor,
                                                                                                this,
                                                                                                tiledArgs,
                                                                                                frameArgs,
                                                                                                tlsCopy,
                                                                                                _1) );
            ret.waitForFinished();
#endif
            ///never call endsequence render here if the render is sequential

            if (callBegin) {
                assert( !( (supportsRenderScaleMaybe() == eSupportsNo) && !(renderMappedScale.x == 1. && renderMappedScale.y == 1.) ) );
                if (endSequenceRender_public(time, time, time, false, renderMappedScale,
                                             isSequentialRender,
                                             isRenderMadeInResponseToUserInteraction,
                                             view) == eStatusFailed) {
                    renderStatus = eStatusFailed;
                    break;
                }
            }
            
#ifdef NATRON_HOSTFRAMETHREADING_SEQUENTIAL
            std::vector<EffectInstance::RenderingFunctorRetEnum>::const_iterator it2;
#else
            QFuture<EffectInstance::RenderingFunctorRetEnum>::const_iterator it2;
#endif
            for (it2 = ret.begin(); it2 != ret.end(); ++it2) {
                if ( (*it2) == EffectInstance::eRenderingFunctorRetFailed ) {
                    renderStatus = eStatusFailed;
                    break;
                }
#if NATRON_ENABLE_TRIMAP
                else if ((*it2) == EffectInstance::eRenderingFunctorRetTakeImageLock) {
                    planesToRender.isBeingRenderedElsewhere = true;
                }
#endif  
                else if ((*it2) == EffectInstance::eRenderingFunctorRetAborted) {
                    renderStatus = eStatusFailed;
                    break;
                }
            }
            break;
        }

        case eRenderSafetyInstanceSafe:     // indicating that any instance can have a single 'render' call at any one time,
        case eRenderSafetyFullySafe:        // indicating that any instance of a plugin can have multiple renders running simultaneously
        case eRenderSafetyUnsafe: {     // indicating that only a single 'render' call can be made at any time amoung all instances
            
            
            RenderingFunctorRetEnum functorRet = tiledRenderingFunctor(args,
                                                                       frameArgs,
                                                                       *inputImgIt,
                                                                       tlsCopy,
                                                                       renderFullScaleThenDownscale,
                                                                       useScaleOneInputImages,
                                                                       isSequentialRender,
                                                                       isRenderMadeInResponseToUserInteraction,
                                                                       downscaledRectToRender,
                                                                       par,
                                                                       planesToRender);
            

            
            if (functorRet == eRenderingFunctorRetFailed) {
                renderStatus = eStatusFailed;
            } else if (functorRet == eRenderingFunctorRetOK) {
                renderStatus = eStatusOK;
            } else if  (functorRet == eRenderingFunctorRetTakeImageLock) {
                renderStatus = eStatusOK;
#if NATRON_ENABLE_TRIMAP
                planesToRender.isBeingRenderedElsewhere = true;
#endif
            } else if (functorRet == eRenderingFunctorRetAborted) {
                renderStatus = eStatusFailed;
            }
            
            break;
        }
        } // switch
 
        

        if (renderStatus != eStatusOK) {
            break;
        }
    } // for (std::list<RectI>::const_iterator it = rectsToRender.begin(); it != rectsToRender.end(); ++it) {
    
    
    if (renderStatus != eStatusOK) {
        retCode = eRenderRoIStatusRenderFailed;
    }

    return retCode;
} // renderRoIInternal

EffectInstance::RenderingFunctorRetEnum
EffectInstance::tiledRenderingFunctor(const TiledRenderingFunctorArgs& args,
                                      const ParallelRenderArgs& frameArgs,
                                     const std::map<boost::shared_ptr<Natron::Node>,ParallelRenderArgs >& frameTLS,
                                     const RectI & downscaledRectToRender )
{
    return tiledRenderingFunctor(*args.args,
                                 frameArgs,
                                 args.inputImages,
                                 frameTLS,
                                 args.renderFullScaleThenDownscale,
                                 args.renderUseScaleOneInputs,
                                 args.isSequentialRender,
                                 args.isRenderResponseToUserInteraction,
                                 downscaledRectToRender,
                                 args.par,
                                 *args.planes);
}

EffectInstance::RenderingFunctorRetEnum
EffectInstance::tiledRenderingFunctor(RenderArgs & args,
                                      const ParallelRenderArgs& frameArgs,
                                      const std::list<boost::shared_ptr<Natron::Image> >& inputImages,
                                      const std::map<boost::shared_ptr<Natron::Node>,ParallelRenderArgs >& frameTLS,
                                      bool renderFullScaleThenDownscale,
                                      bool renderUseScaleOneInputs,
                                      bool isSequentialRender,
                                      bool isRenderResponseToUserInteraction,
                                      const RectI & downscaledRectToRender,
                                      const double par,
                                      ImagePlanesToRender& planes)
{
    
    const PlaneToRender& firstPlane = planes.planes.begin()->second;
    
    const SequenceTime time = args._time;
    int mipMapLevel = firstPlane.downscaleImage->getMipMapLevel();
    const int view = args._view;

    // at this point, it may be unnecessary to call render because it was done a long time ago => check the bitmap here!
# ifndef NDEBUG
    RectI  renderBounds = firstPlane.renderMappedImage->getBounds();
    assert(renderBounds.x1 <= downscaledRectToRender.x1 && downscaledRectToRender.x2 <= renderBounds.x2 &&
           renderBounds.y1 <= downscaledRectToRender.y1 && downscaledRectToRender.y2 <= renderBounds.y2);
# endif
    
   
    
    
    RectI renderRectToRender; // rectangle to render, in renderMappedImage's pixel coordinates
    
    RenderScale renderMappedScale;
    renderMappedScale.x = renderMappedScale.y = Image::getScaleFromMipMapLevel( firstPlane.renderMappedImage->getMipMapLevel() );
    assert( !( (supportsRenderScaleMaybe() == eSupportsNo) && !(renderMappedScale.x == 1. && renderMappedScale.y == 1.) ) );
    
    
    ///Make the thread-storage live as long as the render action is called if we're in a newly launched thread in eRenderSafetyFullySafeFrame mode
    boost::shared_ptr<Implementation::ScopedRenderArgs> scopedArgs;
    boost::shared_ptr<ParallelRenderArgsSetter> scopedFrameArgs;
    
    boost::shared_ptr<InputImagesHolder_RAII> scopedInputImages;
    
    ImageList tmpPlanes;
    
    bool isBeingRenderedElseWhere = false;
    if (frameTLS.empty()) {
        renderRectToRender = args._renderWindowPixel;
        
        for (std::map<Natron::ImageComponents, PlaneToRender>::iterator it = planes.planes.begin(); it != planes.planes.end(); ++it) {
            /*
             * When using the cache, allocate a local temporary buffer onto which the plug-in will render, and then safely
             * copy this buffer to the shared (among threads) image.
             */
            if (it->second.renderMappedImage->usesBitMap()) {
                it->second.tmpImage.reset(new Image(it->second.renderMappedImage->getComponents(),
                                                    it->second.renderMappedImage->getRoD(),
                                                    renderRectToRender,
                                                    it->second.renderMappedImage->getMipMapLevel(),
                                                    it->second.renderMappedImage->getPixelAspectRatio(),
                                                    it->second.renderMappedImage->getBitDepth(),
                                                    false)); //< no bitmap
                
            } else {
                it->second.tmpImage = it->second.renderMappedImage;
            }
            tmpPlanes.push_back(it->second.tmpImage);
        }
        args._outputPlanes = planes.planes;
    } else {
        
        ///At this point if we're in eRenderSafetyFullySafeFrame mode, we are a thread that might have been launched way after
        ///the time renderRectToRender was computed. We recompute it to update the portion to render.
        ///Note that if it is bigger than the initial rectangle, we don't render the bigger rectangle since we cannot
        ///now make the preliminaries call to handle that region (getRoI etc...) so just stick with the old rect to render
        
        // check the bitmap!
        if (renderFullScaleThenDownscale && renderUseScaleOneInputs) {
            
            //The renderMappedImage is cached , read bitmap from it
            RectD canonicalrenderRectToRender;
            downscaledRectToRender.toCanonical(mipMapLevel, par, args._rod, &canonicalrenderRectToRender);
            canonicalrenderRectToRender.toPixelEnclosing(0, par, &renderRectToRender);
            renderRectToRender.intersect(firstPlane.renderMappedImage->getBounds(), &renderRectToRender);
            
            RectI initialRenderRect = renderRectToRender;
            
#if NATRON_ENABLE_TRIMAP
            if (!frameArgs.canAbort && frameArgs.isRenderResponseToUserInteraction) {
                renderRectToRender = firstPlane.renderMappedImage->getMinimalRect_trimap(renderRectToRender,&isBeingRenderedElseWhere);
            } else {
                renderRectToRender = firstPlane.renderMappedImage->getMinimalRect(renderRectToRender);
            }
#else
            renderRectToRender = renderMappedImage->getMinimalRect(renderRectToRender);
#endif
            
            ///If the new rect after getMinimalRect is bigger (maybe because another thread as grown the image)
            ///we stick to what was requested
            if (!initialRenderRect.contains(renderRectToRender)) {
                renderRectToRender = initialRenderRect;
            }
            
            assert(renderBounds.x1 <= renderRectToRender.x1 && renderRectToRender.x2 <= renderBounds.x2 &&
                   renderBounds.y1 <= renderRectToRender.y1 && renderRectToRender.y2 <= renderBounds.y2);
        } else {
            //The downscaled image is cached, read bitmap from it
#if NATRON_ENABLE_TRIMAP
            RectI downscaledRectToRenderMinimal;
            if (!frameArgs.canAbort && frameArgs.isRenderResponseToUserInteraction) {
                downscaledRectToRenderMinimal = firstPlane.downscaleImage->getMinimalRect_trimap(downscaledRectToRender,&isBeingRenderedElseWhere);
            } else {
                downscaledRectToRenderMinimal = firstPlane.downscaleImage->getMinimalRect(downscaledRectToRender);
            }
#else
            const RectI downscaledRectToRenderMinimal = downscaledImage->getMinimalRect(downscaledRectToRender);
#endif
            
            assert(renderBounds.x1 <= downscaledRectToRenderMinimal.x1 && downscaledRectToRenderMinimal.x2 <= renderBounds.x2 &&
                   renderBounds.y1 <= downscaledRectToRenderMinimal.y1 && downscaledRectToRenderMinimal.y2 <= renderBounds.y2);
            
            
            
            if (renderFullScaleThenDownscale) {

                
                ///If the new rect after getMinimalRect is bigger (maybe because another thread as grown the image)
                ///we stick to what was requested
                if (downscaledRectToRender.contains(downscaledRectToRenderMinimal)) {
                    RectD canonicalrenderRectToRender;
                    downscaledRectToRenderMinimal.toCanonical(mipMapLevel, par, args._rod, &canonicalrenderRectToRender);
                    canonicalrenderRectToRender.toPixelEnclosing(0, par, &renderRectToRender);
                    renderRectToRender.intersect(firstPlane.renderMappedImage->getBounds(), &renderRectToRender);
                } else {
                    RectD canonicalrenderRectToRender;
                    downscaledRectToRender.toCanonical(mipMapLevel, par, args._rod, &canonicalrenderRectToRender);
                    canonicalrenderRectToRender.toPixelEnclosing(0, par, &renderRectToRender);
                    renderRectToRender.intersect(firstPlane.renderMappedImage->getBounds(), &renderRectToRender);
                }
            } else {
                
                ///If the new rect after getMinimalRect is bigger (maybe because another thread as grown the image)
                ///we stick to what was requested
                if (downscaledRectToRender.contains(downscaledRectToRenderMinimal)) {
                    renderRectToRender = downscaledRectToRenderMinimal;
                } else {
                    renderRectToRender = downscaledRectToRender;
                }
            }
            
        }
        
        if ( renderRectToRender.isNull() ) {
            ///We've got nothing to do
            return isBeingRenderedElseWhere ? eRenderingFunctorRetTakeImageLock : eRenderingFunctorRetOK;
        }
        
        RenderArgs argsCpy(args);
        ///Update the renderWindow which might have changed
        argsCpy._renderWindowPixel = renderRectToRender;
        argsCpy._outputPlanes = planes.planes;
        
        for (std::map<Natron::ImageComponents, PlaneToRender>::iterator it = argsCpy._outputPlanes.begin();
             it != argsCpy._outputPlanes.end(); ++it) {
            /*
             * When using the cache, allocate a local temporary buffer onto which the plug-in will render, and then safely
             * copy this buffer to the shared (among threads) image.
             */
            if (it->second.renderMappedImage->usesBitMap()) {
                it->second.tmpImage.reset(new Image(it->second.renderMappedImage->getComponents(),
                                                    it->second.renderMappedImage->getRoD(),
                                                    renderRectToRender,
                                                    it->second.renderMappedImage->getMipMapLevel(),
                                                    it->second.renderMappedImage->getPixelAspectRatio(),
                                                    it->second.renderMappedImage->getBitDepth(),
                                                    false)); //< no bitmap
                
            } else {
                it->second.tmpImage = it->second.renderMappedImage;
            }
            tmpPlanes.push_back(it->second.tmpImage);
        }
        
        scopedArgs.reset( new Implementation::ScopedRenderArgs(&_imp->renderArgs,argsCpy) );
        scopedFrameArgs.reset( new ParallelRenderArgsSetter(frameTLS));
        
        scopedInputImages.reset(new InputImagesHolder_RAII(inputImages,&_imp->inputImages));
    }
    
    
#if NATRON_ENABLE_TRIMAP
    if (!frameArgs.canAbort && frameArgs.isRenderResponseToUserInteraction) {
        if (renderFullScaleThenDownscale && renderUseScaleOneInputs) {
            firstPlane.fullscaleImage->markForRendering(renderRectToRender);
        } else {
            firstPlane.downscaleImage->markForRendering(downscaledRectToRender);
        }
    }
#endif
    
    /// Render in the temporary image
    
    RenderScale originalScale;
    originalScale.x = firstPlane.downscaleImage->getScale();
    originalScale.y = originalScale.x;

    Natron::StatusEnum st = render_public(time,
                                          originalScale,
                                          renderMappedScale,
                                          renderRectToRender, view,
                                          isSequentialRender,
                                          isRenderResponseToUserInteraction,
                                          tmpPlanes);
    
    bool renderAborted = aborted();
    
    /*
     * Since new planes can have been allocated on the fly by allocateImagePlaneAndSetInThreadLocalStorage(), refresh
     * the planes map from the thread local storage.
     */
    assert(_imp->renderArgs.hasLocalData());
    const RenderArgs& curRenderArgs = _imp->renderArgs.localData();
    assert(curRenderArgs._validArgs);

    std::map<Natron::ImageComponents,PlaneToRender> outputPlanes = curRenderArgs._outputPlanes;
    assert(!outputPlanes.empty());
    
    if (st != eStatusOK) {
#if NATRON_ENABLE_TRIMAP
        if (!frameArgs.canAbort && frameArgs.isRenderResponseToUserInteraction) {
            assert(!renderAborted);
            
            for (std::map<ImageComponents,PlaneToRender>::const_iterator it = outputPlanes.begin(); it!=outputPlanes.end(); ++it) {
                if (renderFullScaleThenDownscale && renderUseScaleOneInputs) {
                    it->second.fullscaleImage->clearBitmap(renderRectToRender);
                } else {
                    it->second.downscaleImage->clearBitmap(downscaledRectToRender);
                }
            }
            
        }
#endif
        return eRenderingFunctorRetFailed;
        
    }
    
    if (renderAborted) {
        return eRenderingFunctorRetAborted;
    } else {
    
        //Check for NaNs
        for (std::map<ImageComponents,PlaneToRender>::const_iterator it = outputPlanes.begin(); it!=outputPlanes.end(); ++it) {
            if (it->second.tmpImage->checkForNaNs(renderRectToRender)) {
                qDebug() << getNode()->getScriptName_mt_safe().c_str() << ": rendered rectangle (" << renderRectToRender.x1 << ',' << renderRectToRender.y1 << ")-(" << renderRectToRender.x2 << ',' << renderRectToRender.y2 << ") contains invalid values.";
            }
            
            if (it->second.isAllocatedOnTheFly) {
                ///Plane allocated on the fly only have a temp image if using the cache and it is defined over the render window only
                if (it->second.tmpImage != it->second.renderMappedImage) {
                    assert(it->second.tmpImage->getBounds() == renderRectToRender);
                    it->second.renderMappedImage->pasteFrom(*(it->second.tmpImage), it->second.tmpImage->getBounds(), false);
                }
                it->second.renderMappedImage->markForRendered(renderRectToRender);
                
            } else {
                
                ///copy the rectangle rendered in the full scale image to the downscaled output
                if (renderFullScaleThenDownscale) {
                    
                    ///If we're using renderUseScaleOneInputs, the full scale image is cached, hence we're not sure that the whole part of the image will be downscaled.
                    ///Instead we do all the downscale at once at the end of renderRoI().
                    ///If !renderUseScaleOneInputs the image is not cached and we know it will be rendered completly so it is safe to do this here and take advantage
                    ///of the multi-threading.
                    if (mipMapLevel != 0 && !renderUseScaleOneInputs) {
                        assert(it->second.fullscaleImage != it->second.downscaleImage && it->second.renderMappedImage == it->second.fullscaleImage);
                        it->second.tmpImage->downscaleMipMap(it->second.tmpImage->getRoD(),
                                                             renderRectToRender, 0, mipMapLevel, false,it->second.downscaleImage.get() );
                        it->second.downscaleImage->markForRendered(downscaledRectToRender);
                    } else {
                        assert(it->second.renderMappedImage == it->second.fullscaleImage);
                        if (it->second.tmpImage != it->second.renderMappedImage) {
                            it->second.fullscaleImage->pasteFrom(*it->second.tmpImage, renderRectToRender, false);
                        }
                        it->second.fullscaleImage->markForRendered(renderRectToRender);
                    }
                } else {
                    if (it->second.tmpImage != it->second.downscaleImage) {
                        it->second.downscaleImage->pasteFrom(*it->second.tmpImage, downscaledRectToRender,false);
                    }
                    it->second.downscaleImage->markForRendered(downscaledRectToRender);
                }
            }
        }
        
    }
  
    
    return isBeingRenderedElseWhere ? eRenderingFunctorRetTakeImageLock : eRenderingFunctorRetOK;
} // tiledRenderingFunctor

ImagePtr
EffectInstance::allocateImagePlaneAndSetInThreadLocalStorage(const Natron::ImageComponents& plane)
{
    /*
     * The idea here is that we may have asked the plug-in to render say motion.forward, but it can only render both fotward 
     * and backward at a time.
     * So it needs to allocate motion.backward and store it in the cache for efficiency. 
     * Note that when calling this, the plug-in is already in the render action, hence in case of Host frame threading, 
     * this function will be called as many times as there were thread used by the host frame threading.
     * For all other planes, there was a local temporary image, shared among all threads for the calls to render.
     * Since we may be in a thread of the host frame threading, only allocate a temporary image of the size of the rectangle
     * to render and mark that we're a plane allocated on the fly so that the tiledRenderingFunctor can know this is a plane
     * to handle specifically.
     */
    
    if (_imp->renderArgs.hasLocalData()) {
        RenderArgs& args = _imp->renderArgs.localData();
        if (args._validArgs) {
            
            assert(!args._outputPlanes.empty());
            
            const PlaneToRender& firstPlane = args._outputPlanes.begin()->second;
            
            bool useCache = firstPlane.fullscaleImage->usesBitMap() || firstPlane.downscaleImage->usesBitMap();
            
            const ImagePtr& img = firstPlane.fullscaleImage->usesBitMap() ? firstPlane.fullscaleImage : firstPlane.downscaleImage;
            
            boost::shared_ptr<ImageParams> params = img->getParams();
            
            PlaneToRender p;
            bool ok = allocateImagePlane(img->getKey(), img->getRoD(), img->getBounds(), img->getBounds(), false, params->getFramesNeeded(), plane, img->getBitDepth(), img->getPixelAspectRatio(), img->getMipMapLevel(), false, false, false, useCache, &p.fullscaleImage, &p.downscaleImage);
            if (!ok) {
                return ImagePtr();
            } else {
                
                p.renderMappedImage = p.downscaleImage;
                p.isAllocatedOnTheFly = true;
                
                /*
                 * Allocate a temporary image for rendering only if using cache
                 */
                if (useCache) {
                    p.tmpImage.reset(new Image(p.renderMappedImage->getComponents(),
                                               p.renderMappedImage->getRoD(),
                                               args._renderWindowPixel,
                                               p.renderMappedImage->getMipMapLevel(),
                                               p.renderMappedImage->getPixelAspectRatio(),
                                               p.renderMappedImage->getBitDepth(),
                                               false));
                } else {
                    p.tmpImage = p.renderMappedImage;
                }
                args._outputPlanes.insert(std::make_pair(plane,p));
                return p.downscaleImage;
            }
            
        } else {
            return ImagePtr();
        }
    } else {
        return ImagePtr();
    }
} // allocateImagePlaneAndSetInThreadLocalStorage

void
EffectInstance::openImageFileKnob()
{
    const std::vector< boost::shared_ptr<KnobI> > & knobs = getKnobs();

    for (U32 i = 0; i < knobs.size(); ++i) {
        if ( knobs[i]->typeName() == File_Knob::typeNameStatic() ) {
            boost::shared_ptr<File_Knob> fk = boost::dynamic_pointer_cast<File_Knob>(knobs[i]);
            assert(fk);
            if ( fk->isInputImageFile() ) {
                std::string file = fk->getValue();
                if ( file.empty() ) {
                    fk->open_file();
                }
                break;
            }
        } else if ( knobs[i]->typeName() == OutputFile_Knob::typeNameStatic() ) {
            boost::shared_ptr<OutputFile_Knob> fk = boost::dynamic_pointer_cast<OutputFile_Knob>(knobs[i]);
            assert(fk);
            if ( fk->isOutputImageFile() ) {
                std::string file = fk->getValue();
                if ( file.empty() ) {
                    fk->open_file();
                }
                break;
            }
        }
    }
}


void
EffectInstance::evaluate(KnobI* knob,
                         bool isSignificant,
                         Natron::ValueChangedReasonEnum /*reason*/)
{

    ////If the node is currently modifying its input, to ask for a render
    ////because at then end of the inputChanged handler, it will ask for a refresh
    ////and a rebuild of the inputs tree.
    NodePtr node = getNode();
    if ( node->duringInputChangedAction() ) {
        return;
    }

    if ( getApp()->getProject()->isLoadingProject() ) {
        return;
    }


    Button_Knob* button = dynamic_cast<Button_Knob*>(knob);

    /*if this is a writer (openfx or built-in writer)*/
    if ( isWriter() ) {
        /*if this is a button and it is a render button,we're safe to assume the plug-ins wants to start rendering.*/
        if (button) {
            if ( button->isRenderButton() ) {
                std::string sequentialNode;
                if ( node->hasSequentialOnlyNodeUpstream(sequentialNode) ) {
                    if (node->getApp()->getProject()->getProjectViewsCount() > 1) {
                        Natron::StandardButtonEnum answer =
                        Natron::questionDialog( QObject::tr("Render").toStdString(),
                                               sequentialNode + QObject::tr(" can only "
                                                                            "render in sequential mode. Due to limitations in the "
                                                                            "OpenFX standard that means that %1"
                                                                            " will not be able "
                                                                            "to render all the views of the project. "
                                                                            "Only the main view of the project will be rendered, you can "
                                                                            "change the main view in the project settings. Would you like "
                                                                            "to continue ?").arg(NATRON_APPLICATION_NAME).toStdString(),false );
                        if (answer != Natron::eStandardButtonYes) {
                            return;
                        }
                    }
                }
                AppInstance::RenderWork w;
                w.writer = dynamic_cast<OutputEffectInstance*>(this);
                w.firstFrame = INT_MIN;
                w.lastFrame = INT_MAX;
                std::list<AppInstance::RenderWork> works;
                works.push_back(w);
                getApp()->startWritersRendering(works);

                return;
            }
        }
    }

    ///increments the knobs age following a change
    if (!button && isSignificant) {
        node->incrementKnobsAge();
    }
    
    
    int time = getCurrentTime();
    
    
    std::list<ViewerInstance* > viewers;
    node->hasViewersConnected(&viewers);
    for (std::list<ViewerInstance* >::iterator it = viewers.begin();
         it != viewers.end();
         ++it) {
        if (isSignificant) {
            (*it)->renderCurrentFrame(true);
        } else {
            (*it)->redrawViewer();
        }
    }
    
    getNode()->refreshPreviewsRecursivelyDownstream(time);
} // evaluate

bool
EffectInstance::message(Natron::MessageTypeEnum type,
                        const std::string & content) const
{
    return getNode()->message(type,content);
}

void
EffectInstance::setPersistentMessage(Natron::MessageTypeEnum type,
                                     const std::string & content)
{
    getNode()->setPersistentMessage(type, content);
}

void
EffectInstance::clearPersistentMessage(bool recurse)
{
    getNode()->clearPersistentMessage(recurse);
}

int
EffectInstance::getInputNumber(Natron::EffectInstance* inputEffect) const
{
    for (int i = 0; i < getMaxInputCount(); ++i) {
        if (getInput(i) == inputEffect) {
            return i;
        }
    }

    return -1;
}

/**
 * @brief Does this effect supports rendering at a different scale than 1 ?
 * There is no OFX property for this purpose. The only solution found for OFX is that if a isIdentity
 * with renderscale != 1 fails, the host retries with renderscale = 1 (and upscaled images).
 * If the renderScale support was not set, this throws an exception.
 **/
bool
EffectInstance::supportsRenderScale() const
{
    if (_imp->supportsRenderScale == eSupportsMaybe) {
        qDebug() << "EffectInstance::supportsRenderScale should be set before calling supportsRenderScale(), or use supportsRenderScaleMaybe() instead";
        throw std::runtime_error("supportsRenderScale not set");
    }

    return _imp->supportsRenderScale == eSupportsYes;
}

EffectInstance::SupportsEnum
EffectInstance::supportsRenderScaleMaybe() const
{
    QMutexLocker l(&_imp->supportsRenderScaleMutex);

    return _imp->supportsRenderScale;
}

/// should be set during effect initialization, but may also be set by the first getRegionOfDefinition that succeeds
void
EffectInstance::setSupportsRenderScaleMaybe(EffectInstance::SupportsEnum s) const
{
    {
        QMutexLocker l(&_imp->supportsRenderScaleMutex);
        
        _imp->supportsRenderScale = s;
    }
    NodePtr node = getNode();
    if (node) {
        node->onSetSupportRenderScaleMaybeSet((int)s);
    }
}

void
EffectInstance::setOutputFilesForWriter(const std::string & pattern)
{
    if ( !isWriter() ) {
        return;
    }

    const std::vector<boost::shared_ptr<KnobI> > & knobs = getKnobs();
    for (U32 i = 0; i < knobs.size(); ++i) {
        if ( knobs[i]->typeName() == OutputFile_Knob::typeNameStatic() ) {
            boost::shared_ptr<OutputFile_Knob> fk = boost::dynamic_pointer_cast<OutputFile_Knob>(knobs[i]);
            assert(fk);
            if ( fk->isOutputImageFile() ) {
                fk->setValue(pattern,0);
                break;
            }
        }
    }
}

PluginMemory*
EffectInstance::newMemoryInstance(size_t nBytes)
{
    PluginMemory* ret = new PluginMemory( getNode()->getLiveInstance() ); //< hack to get "this" as a shared ptr
    bool wasntLocked = ret->alloc(nBytes);

    assert(wasntLocked);
    (void)wasntLocked;

    return ret;
}

void
EffectInstance::addPluginMemoryPointer(PluginMemory* mem)
{
    QMutexLocker l(&_imp->pluginMemoryChunksMutex);

    _imp->pluginMemoryChunks.push_back(mem);
}

void
EffectInstance::removePluginMemoryPointer(PluginMemory* mem)
{
    QMutexLocker l(&_imp->pluginMemoryChunksMutex);
    std::list<PluginMemory*>::iterator it = std::find(_imp->pluginMemoryChunks.begin(),_imp->pluginMemoryChunks.end(),mem);

    if ( it != _imp->pluginMemoryChunks.end() ) {
        _imp->pluginMemoryChunks.erase(it);
    }
}

void
EffectInstance::registerPluginMemory(size_t nBytes)
{
    getNode()->registerPluginMemory(nBytes);
}

void
EffectInstance::unregisterPluginMemory(size_t nBytes)
{
    getNode()->unregisterPluginMemory(nBytes);
}

void
EffectInstance::onAllKnobsSlaved(bool isSlave,
                                 KnobHolder* master)
{
    getNode()->onAllKnobsSlaved(isSlave,master);
}

void
EffectInstance::onKnobSlaved(KnobI* slave,KnobI* master,
                             int dimension,
                             bool isSlave)
{
    getNode()->onKnobSlaved(slave,master,dimension,isSlave);
}

void
EffectInstance::setCurrentViewportForOverlays_public(OverlaySupport* viewport)
{
    getNode()->setCurrentViewportForDefaultOverlays(viewport);
    setCurrentViewportForOverlays(viewport);
}

void
EffectInstance::drawOverlay_public(double scaleX,
                                   double scaleY)
{
    ///cannot be run in another thread
    assert( QThread::currentThread() == qApp->thread() );
    if ( !hasOverlay() && !getNode()->hasDefaultOverlay() ) {
        return;
    }

    RECURSIVE_ACTION();

    _imp->setDuringInteractAction(true);
    drawOverlay(scaleX,scaleY);
    getNode()->drawDefaultOverlay(scaleX, scaleY);
    _imp->setDuringInteractAction(false);
}

bool
EffectInstance::onOverlayPenDown_public(double scaleX,
                                        double scaleY,
                                        const QPointF & viewportPos,
                                        const QPointF & pos)
{
    ///cannot be run in another thread
    assert( QThread::currentThread() == qApp->thread() );
    if ( !hasOverlay()  && !getNode()->hasDefaultOverlay() ) {
        return false;
    }
    
    bool ret;
    {
        NON_RECURSIVE_ACTION();
        _imp->setDuringInteractAction(true);
        ret = onOverlayPenDown(scaleX,scaleY,viewportPos, pos);
        if (!ret) {
            ret |= getNode()->onOverlayPenDownDefault(scaleX, scaleY, viewportPos, pos);
        }
        _imp->setDuringInteractAction(false);
    }
    checkIfRenderNeeded();
    
    return ret;
}

bool
EffectInstance::onOverlayPenMotion_public(double scaleX,
                                          double scaleY,
                                          const QPointF & viewportPos,
                                          const QPointF & pos)
{
    ///cannot be run in another thread
    assert( QThread::currentThread() == qApp->thread() );
    if ( !hasOverlay()  && !getNode()->hasDefaultOverlay() ) {
        return false;
    }
    

    NON_RECURSIVE_ACTION();
    _imp->setDuringInteractAction(true);
    bool ret = onOverlayPenMotion(scaleX,scaleY,viewportPos, pos);
    if (!ret) {
        ret |= getNode()->onOverlayPenMotionDefault(scaleX, scaleY, viewportPos, pos);
    }
    _imp->setDuringInteractAction(false);
    //Don't chek if render is needed on pen motion, wait for the pen up

    //checkIfRenderNeeded();
    return ret;
}

bool
EffectInstance::onOverlayPenUp_public(double scaleX,
                                      double scaleY,
                                      const QPointF & viewportPos,
                                      const QPointF & pos)
{
    ///cannot be run in another thread
    assert( QThread::currentThread() == qApp->thread() );
    if ( !hasOverlay()  && !getNode()->hasDefaultOverlay() ) {
        return false;
    }
    bool ret;
    {
        NON_RECURSIVE_ACTION();
        _imp->setDuringInteractAction(true);
        ret = onOverlayPenUp(scaleX,scaleY,viewportPos, pos);
        if (!ret) {
            ret |= getNode()->onOverlayPenUpDefault(scaleX, scaleY, viewportPos, pos);
        }
        _imp->setDuringInteractAction(false);
    }
    checkIfRenderNeeded();
    
    return ret;
}

bool
EffectInstance::onOverlayKeyDown_public(double scaleX,
                                        double scaleY,
                                        Natron::Key key,
                                        Natron::KeyboardModifiers modifiers)
{
    ///cannot be run in another thread
    assert( QThread::currentThread() == qApp->thread() );
    if ( !hasOverlay()  && !getNode()->hasDefaultOverlay() ) {
        return false;
    }

    bool ret;
    {
        NON_RECURSIVE_ACTION();
        _imp->setDuringInteractAction(true);
        ret = onOverlayKeyDown(scaleX,scaleY,key, modifiers);
        if (!ret) {
            ret |= getNode()->onOverlayKeyDownDefault(scaleX, scaleY, key, modifiers);
        }
        _imp->setDuringInteractAction(false);
    }
    checkIfRenderNeeded();
    
    return ret;
}

bool
EffectInstance::onOverlayKeyUp_public(double scaleX,
                                      double scaleY,
                                      Natron::Key key,
                                      Natron::KeyboardModifiers modifiers)
{
    ///cannot be run in another thread
    assert( QThread::currentThread() == qApp->thread() );
    if ( !hasOverlay()  && !getNode()->hasDefaultOverlay() ) {
        return false;
    }
    
    bool ret;
    {
        NON_RECURSIVE_ACTION();
        
        _imp->setDuringInteractAction(true);
        ret = onOverlayKeyUp(scaleX, scaleY, key, modifiers);
        if (!ret) {
            ret |= getNode()->onOverlayKeyUpDefault(scaleX, scaleY, key, modifiers);
        }
        _imp->setDuringInteractAction(false);
    }
    checkIfRenderNeeded();

    return ret;
}

bool
EffectInstance::onOverlayKeyRepeat_public(double scaleX,
                                          double scaleY,
                                          Natron::Key key,
                                          Natron::KeyboardModifiers modifiers)
{
    ///cannot be run in another thread
    assert( QThread::currentThread() == qApp->thread() );
    if ( !hasOverlay()  && !getNode()->hasDefaultOverlay() ) {
        return false;
    }
    
    bool ret;
    {
        NON_RECURSIVE_ACTION();
        _imp->setDuringInteractAction(true);
        ret = onOverlayKeyRepeat(scaleX,scaleY,key, modifiers);
        if (!ret) {
            ret |= getNode()->onOverlayKeyRepeatDefault(scaleX, scaleY, key, modifiers);
        }
        _imp->setDuringInteractAction(false);
    }
    checkIfRenderNeeded();
    
    return ret;
}

bool
EffectInstance::onOverlayFocusGained_public(double scaleX,
                                            double scaleY)
{
    ///cannot be run in another thread
    assert( QThread::currentThread() == qApp->thread() );
    if ( !hasOverlay() && !getNode()->hasDefaultOverlay()  ) {
        return false;
    }
    
    bool ret;
    {
        NON_RECURSIVE_ACTION();
        _imp->setDuringInteractAction(true);
        ret = onOverlayFocusGained(scaleX,scaleY);
        if (!ret) {
            ret |= getNode()->onOverlayFocusGainedDefault(scaleX, scaleY);
        }
        _imp->setDuringInteractAction(false);
    }
    checkIfRenderNeeded();
    
    return ret;
}

bool
EffectInstance::onOverlayFocusLost_public(double scaleX,
                                          double scaleY)
{
    ///cannot be run in another thread
    assert( QThread::currentThread() == qApp->thread() );
    if ( !hasOverlay() && !getNode()->hasDefaultOverlay()  ) {
        return false;
    }
    bool ret;
    {
        
        NON_RECURSIVE_ACTION();
        _imp->setDuringInteractAction(true);
        ret = onOverlayFocusLost(scaleX,scaleY);
        if (!ret) {
            ret |= getNode()->onOverlayFocusLostDefault(scaleX, scaleY);
        }
        _imp->setDuringInteractAction(false);
    }
    checkIfRenderNeeded();
    
    return ret;
}

bool
EffectInstance::isDoingInteractAction() const
{
    QReadLocker l(&_imp->duringInteractActionMutex);

    return _imp->duringInteractAction;
}

Natron::StatusEnum
EffectInstance::render_public(SequenceTime time,
                              const RenderScale& originalScale,
                              const RenderScale & mappedScale,
                              const RectI & roi,
                              int view,
                              bool isSequentialRender,
                              bool isRenderResponseToUserInteraction,
                              const std::list<boost::shared_ptr<Natron::Image> >& outputPlanes)
{
    NON_RECURSIVE_ACTION();
    return render(time, originalScale, mappedScale, roi, view, isSequentialRender, isRenderResponseToUserInteraction, outputPlanes);

}

Natron::StatusEnum
EffectInstance::getTransform_public(SequenceTime time,
                                    const RenderScale& renderScale,
                                    int view,
                                    Natron::EffectInstance** inputToTransform,
                                    Transform::Matrix3x3* transform)
{
    RECURSIVE_ACTION();
    assert(getCanTransform());
    return getTransform(time, renderScale, view, inputToTransform, transform);
}

bool
EffectInstance::isIdentity_public(U64 hash,
                                  SequenceTime time,
                                  const RenderScale & scale,
                                  const RectD& rod,
                                  const double par,
                                  int view,
                                  SequenceTime* inputTime,
                                  int* inputNb)
{
    
    assert( !( (supportsRenderScaleMaybe() == eSupportsNo) && !(scale.x == 1. && scale.y == 1.) ) );

    unsigned int mipMapLevel = Image::getLevelFromScale(scale.x);
    
    double timeF = 0.;
    bool foundInCache = _imp->actionsCache.getIdentityResult(hash, time, view, mipMapLevel, inputNb, &timeF);
    if (foundInCache) {
        *inputTime = timeF;
        return *inputNb >= 0 || *inputNb == -2;
    } else {
        
        ///If this is running on a render thread, attempt to find the info in the thread local storage.
        if (QThread::currentThread() != qApp->thread() && _imp->renderArgs.hasLocalData()) {
            const RenderArgs& args = _imp->renderArgs.localData();
            if (args._validArgs) {
                *inputNb = args._identityInputNb;
                *inputTime = args._identityTime;
                return *inputNb != -1 ;
            }
        }
        
        ///EDIT: We now allow isIdentity to be called recursively.
        RECURSIVE_ACTION();
        
        ///Lock actions for unsafe plug-ins
        boost::shared_ptr<QMutexLocker> locker;
        if (renderThreadSafety() == eRenderSafetyUnsafe) {
            const Natron::Plugin* p = getNode()->getPlugin();
            assert(p);
            locker.reset(new QMutexLocker(p->getPluginLock()));
        }
        
        bool ret = false;
        
        if (appPTR->isBackground() && dynamic_cast<DiskCacheNode*>(this) != NULL) {
            ret = true;
            *inputNb = 0;
            *inputTime = time;
        } else if ( getNode()->isNodeDisabled() ) {
            
            ret = true;
            *inputTime = time;
            *inputNb = -1;
            *inputNb = getNode()->getPreferredInput();

        } else {
            /// Don't call isIdentity if plugin is sequential only.
            if (getSequentialPreference() != Natron::eSequentialPreferenceOnlySequential) {
                try {
                    ret = isIdentity(time, scale,rod, par, view, inputTime, inputNb);
                } catch (...) {
                    throw;
                }
            }
        }
        if (!ret) {
            *inputNb = -1;
            *inputTime = time;
        }
        _imp->actionsCache.setIdentityResult(time, view, mipMapLevel, *inputNb, *inputTime);
        return ret;
    }
}

void
EffectInstance::onInputChanged(int /*inputNo*/)
{
    if ( !getApp()->getProject()->isLoadingProject() ) {
        RenderScale s;
        s.x = s.y = 1.;
        checkOFXClipPreferences_public(getCurrentTime(), s, kOfxChangeUserEdited,true, true);
    }
}

Natron::StatusEnum
EffectInstance::getRegionOfDefinition_public(U64 hash,
                                             SequenceTime time,
                                             const RenderScale & scale,
                                             int view,
                                             RectD* rod,
                                             bool* isProjectFormat)
{
    if (!isEffectCreated()) {
        return eStatusFailed;
    }
    
    unsigned int mipMapLevel = Image::getLevelFromScale(scale.x);
    bool foundInCache = _imp->actionsCache.getRoDResult(hash, time, view, mipMapLevel, rod);
    if (foundInCache) {
        *isProjectFormat = false;
        if (rod->isNull()) {
            return Natron::eStatusFailed;
        }
        return Natron::eStatusOK;
    } else {
        
        
        ///If this is running on a render thread, attempt to find the RoD in the thread local storage.
        if (QThread::currentThread() != qApp->thread() && _imp->renderArgs.hasLocalData()) {
            const RenderArgs& args = _imp->renderArgs.localData();
            if (args._validArgs) {
                *rod = args._rod;
                *isProjectFormat = false;
                return Natron::eStatusOK;
            }
        }
        
        Natron::StatusEnum ret;
        RenderScale scaleOne;
        scaleOne.x = scaleOne.y = 1.;
        {
            RECURSIVE_ACTION();
            
            ///Lock actions for unsafe plug-ins
            boost::shared_ptr<QMutexLocker> locker;
            if (renderThreadSafety() == eRenderSafetyUnsafe) {
                const Natron::Plugin* p = getNode()->getPlugin();
                assert(p);
                locker.reset(new QMutexLocker(p->getPluginLock()));
            }
            
            ret = getRegionOfDefinition(hash,time, supportsRenderScaleMaybe() == eSupportsNo ? scaleOne : scale, view, rod);
            
            if ( (ret != eStatusOK) && (ret != eStatusReplyDefault) ) {
                // rod is not valid
                _imp->actionsCache.invalidateAll(hash);
                _imp->actionsCache.setRoDResult(time, view, mipMapLevel, RectD());
                return ret;
            }
            
            if (rod->isNull()) {
                _imp->actionsCache.invalidateAll(hash);
                _imp->actionsCache.setRoDResult(time, view, mipMapLevel, RectD());
                return eStatusFailed;
            }
            
            assert( (ret == eStatusOK || ret == eStatusReplyDefault) && (rod->x1 <= rod->x2 && rod->y1 <= rod->y2) );
            
        }
        *isProjectFormat = ifInfiniteApplyHeuristic(hash,time, scale, view, rod);
        assert(rod->x1 <= rod->x2 && rod->y1 <= rod->y2);

        _imp->actionsCache.setRoDResult( time, view,  mipMapLevel, *rod);
        return ret;
    }
}

void
EffectInstance::getRegionsOfInterest_public(SequenceTime time,
                                            const RenderScale & scale,
                                            const RectD & outputRoD, //!< effect RoD in canonical coordinates
                                            const RectD & renderWindow, //!< the region to be rendered in the output image, in Canonical Coordinates
                                            int view,
                                            EffectInstance::RoIMap* ret)
{
    NON_RECURSIVE_ACTION();
    assert(outputRoD.x2 >= outputRoD.x1 && outputRoD.y2 >= outputRoD.y1);
    assert(renderWindow.x2 >= renderWindow.x1 && renderWindow.y2 >= renderWindow.y1);
    
    ///Lock actions for unsafe plug-ins
    boost::shared_ptr<QMutexLocker> locker;
    if (renderThreadSafety() == eRenderSafetyUnsafe) {
        const Natron::Plugin* p = getNode()->getPlugin();
        assert(p);
        locker.reset(new QMutexLocker(p->getPluginLock()));
    }
    
    getRegionsOfInterest(time, scale, outputRoD, renderWindow, view,ret);
    
}

EffectInstance::FramesNeededMap
EffectInstance::getFramesNeeded_public(SequenceTime time,int view)
{
    NON_RECURSIVE_ACTION();
    
    ///Lock actions for unsafe plug-ins
    boost::shared_ptr<QMutexLocker> locker;
    if (renderThreadSafety() == eRenderSafetyUnsafe) {
        const Natron::Plugin* p = getNode()->getPlugin();
        assert(p);
        locker.reset(new QMutexLocker(p->getPluginLock()));
    }

    return getFramesNeeded(time, view);
}

void
EffectInstance::getFrameRange_public(U64 hash,
                                     SequenceTime *first,
                                     SequenceTime *last,
                                     bool bypasscache)
{
    
    double fFirst = 0.,fLast = 0.;
    bool foundInCache = false;
    if (!bypasscache) {
        foundInCache = _imp->actionsCache.getTimeDomainResult(hash, &fFirst, &fLast);
    }
    if (foundInCache) {
        *first = std::floor(fFirst+0.5);
        *last = std::floor(fLast+0.5);
    } else {
        
        ///If this is running on a render thread, attempt to find the info in the thread local storage.
        if (QThread::currentThread() != qApp->thread() && _imp->renderArgs.hasLocalData()) {
            const RenderArgs& args = _imp->renderArgs.localData();
            if (args._validArgs) {
                *first = args._firstFrame;
                *last = args._lastFrame;
                return;
            }
        }
        
        NON_RECURSIVE_ACTION();
        getFrameRange(first, last);
        _imp->actionsCache.setTimeDomainResult(*first, *last);
    }
}

Natron::StatusEnum
EffectInstance::beginSequenceRender_public(SequenceTime first,
                                           SequenceTime last,
                                           SequenceTime step,
                                           bool interactive,
                                           const RenderScale & scale,
                                           bool isSequentialRender,
                                           bool isRenderResponseToUserInteraction,
                                           int view)
{
    NON_RECURSIVE_ACTION();
    {
        if ( !_imp->beginEndRenderCount.hasLocalData() ) {
            _imp->beginEndRenderCount.localData() = 1;
        } else {
            ++_imp->beginEndRenderCount.localData();
        }
    }

    return beginSequenceRender(first, last, step, interactive, scale,
                               isSequentialRender, isRenderResponseToUserInteraction, view);
}

Natron::StatusEnum
EffectInstance::endSequenceRender_public(SequenceTime first,
                                         SequenceTime last,
                                         SequenceTime step,
                                         bool interactive,
                                         const RenderScale & scale,
                                         bool isSequentialRender,
                                         bool isRenderResponseToUserInteraction,
                                         int view)
{
    NON_RECURSIVE_ACTION();
    {
        assert( _imp->beginEndRenderCount.hasLocalData() );
        --_imp->beginEndRenderCount.localData();
        assert(_imp->beginEndRenderCount.localData() >= 0);
    }
    
    return endSequenceRender(first, last, step, interactive, scale, isSequentialRender, isRenderResponseToUserInteraction, view);
}

bool
EffectInstance::isSupportedComponent(int inputNb,
                                     const Natron::ImageComponents& comp) const
{
    return getNode()->isSupportedComponent(inputNb, comp);
}

Natron::ImageBitDepthEnum
EffectInstance::getBitDepth() const
{
    return getNode()->getBitDepth();
}

bool
EffectInstance::isSupportedBitDepth(Natron::ImageBitDepthEnum depth) const
{
    return getNode()->isSupportedBitDepth(depth);
}

Natron::ImageComponents
EffectInstance::findClosestSupportedComponents(int inputNb,
                                               const Natron::ImageComponents& comp) const
{
    return getNode()->findClosestSupportedComponents(inputNb,comp);
}

void
EffectInstance::getPreferredDepthAndComponents(int inputNb,
                                               std::list<Natron::ImageComponents>* comp,
                                               Natron::ImageBitDepthEnum* depth) const
{
    EffectInstance* inp = 0;
    
    std::list<Natron::ImageComponents> inputComps;
    if (inputNb != -1) {
        inp = getInput(inputNb);
        if (inp) {
            Natron::ImageBitDepthEnum depth;
            inp->getPreferredDepthAndComponents(-1, &inputComps, &depth);
        }
    } else {
        
        int index = getNode()->getPreferredInput();
        if (index != -1) {
            Natron::EffectInstance* input = getInput(index);
            if (input) {
                Natron::ImageBitDepthEnum inputDepth;
                input->getPreferredDepthAndComponents(-1, &inputComps, &inputDepth);

            }
        }
    }
    if (inputComps.empty()) {
        inputComps.push_back(ImageComponents::getNoneComponents());
    }
    for (std::list<Natron::ImageComponents>::iterator it = inputComps.begin(); it!=inputComps.end(); ++it) {
        comp->push_back(findClosestSupportedComponents(inputNb, *it));
    }
    

    ///find deepest bitdepth
    *depth = getBitDepth();
}

void
EffectInstance::getComponentsAvailableRecursive(SequenceTime time, int view, ComponentsAvailableMap* comps,
                                                std::list<Natron::EffectInstance*>* markedNodes)
{
    if (std::find(markedNodes->begin(), markedNodes->end(), this) != markedNodes->end()) {
        return;
    }
    
    ComponentsNeededMap neededComps;
    SequenceTime ptTime;
    int ptView;
    boost::shared_ptr<Natron::Node> ptInput;
    getComponentsNeededAndProduced_public(time, view, &neededComps, &ptTime, &ptView, &ptInput);
    
    ComponentsNeededMap::iterator foundOutput = neededComps.find(-1);
    if (foundOutput != neededComps.end()) {
        
        ///Foreach component produced by the node at the given (view,time),  try
        ///to add it to the components available. Since we are recursing upstream, it is probably
        ///already in there, in which case we ignore it and keep the one from below.
        for (std::vector<Natron::ImageComponents>::iterator it = foundOutput->second.begin();
             it != foundOutput->second.end(); ++it) {
            
            
            ComponentsAvailableMap::iterator alreadyExisting = comps->end();
            
            if (it->isColorPlane()) {
                
                ComponentsAvailableMap::iterator colorMatch = comps->end();
                
                for (ComponentsAvailableMap::iterator it2 = comps->begin(); it2 != comps->end(); ++it2) {
                    if (it2->first == *it) {
                        alreadyExisting = it2;
                        break;
                    } else if (it2->first.isColorPlane()) {
                        colorMatch = it2;
                    }
                }
                
                if (alreadyExisting == comps->end() && colorMatch != comps->end()) {
                    alreadyExisting = colorMatch;
                }
            } else {
                alreadyExisting = comps->find(*it);
            }
            
            //If the component already exists from below in the tree, do not add it
            if (alreadyExisting == comps->end()) {
                comps->insert(std::make_pair(*it, getNode()));
            }
        }
    }
    markedNodes->push_back(this);
    
    
    ///If the plug-in is not pass-through, only consider the components processed by the plug-in in output,
    ///so we do not need to recurse.
    if (isPassThroughForNonRenderedPlanes()) {
        
        bool doHeuristicForPassThrough = false;
        if (isMultiPlanar()) {
            if (!ptInput) {
                doHeuristicForPassThrough = true;
            }
        } else {
            doHeuristicForPassThrough = true;
        }
        
        if (doHeuristicForPassThrough) {
            int inp = getNode()->getPreferredInput();
            ptInput = getNode()->getInput(inp);
        }
        
        if (ptInput) {
            ptInput->getLiveInstance()->getComponentsAvailableRecursive(time, view, comps, markedNodes);
        }
        
    }
}

void
EffectInstance::getComponentsAvailable(SequenceTime time, ComponentsAvailableMap* comps)
{
    int nViews = getApp()->getProject()->getProjectViewsCount();
    
    ///Union components over all views
    for (int view = 0; view < nViews; ++view) {
        std::list<Natron::EffectInstance*> marks;
        getComponentsAvailableRecursive(time, view, comps, &marks);
        
    }
}

void
EffectInstance::getComponentsNeededAndProduced(SequenceTime time, int view,
                                    ComponentsNeededMap* comps,
                                    SequenceTime* passThroughTime,
                                    int* passThroughView,
                                    boost::shared_ptr<Natron::Node>* passThroughInput)
{
    *passThroughTime = time;
    *passThroughView = view;
    
    std::list<Natron::ImageComponents> outputComp;
    Natron::ImageBitDepthEnum outputDepth;
    getPreferredDepthAndComponents(-1, &outputComp , &outputDepth);
    
    std::vector<Natron::ImageComponents> outputCompVec;
    for (std::list<Natron::ImageComponents>::iterator it = outputComp.begin(); it!=outputComp.end(); ++it) {
        outputCompVec.push_back(*it);
    }
    
    
    comps->insert(std::make_pair(-1, outputCompVec));
    
    NodePtr firstConnectedOptional;
    for (int i = 0; i < getMaxInputCount(); ++i) {
        NodePtr node = getNode()->getInput(i);
        if (!node) {
            continue;
        }
        if (isInputRotoBrush(i)) {
            continue;
        }
        
        std::list<Natron::ImageComponents> comp;
        Natron::ImageBitDepthEnum depth;
        getPreferredDepthAndComponents(-1, &comp, &depth);
        
        std::vector<Natron::ImageComponents> compVect;
        for (std::list<Natron::ImageComponents>::iterator it = comp.begin(); it!=comp.end(); ++it) {
            compVect.push_back(*it);
        }
        comps->insert(std::make_pair(i, compVect));
        
        if (!isInputOptional(i)) {
            *passThroughInput = node;
        } else {
            firstConnectedOptional = node;
        }
    }
    if (!*passThroughInput) {
        *passThroughInput = firstConnectedOptional;
    }
    
}

void
EffectInstance::getComponentsNeededAndProduced_public(SequenceTime time, int view,
                                               ComponentsNeededMap* comps,
                                               SequenceTime* passThroughTime,
                                               int* passThroughView,
                                               boost::shared_ptr<Natron::Node>* passThroughInput)

{
    RECURSIVE_ACTION();
    
    if (isMultiPlanar()) {
        getComponentsNeededAndProduced(time, view, comps, passThroughTime, passThroughView, passThroughInput);
    } else {
        *passThroughTime = time;
        *passThroughView = view;
        int idx = getNode()->getPreferredInput();
        *passThroughInput = getNode()->getInput(idx);
        {
            std::vector<ImageComponents> compVec;
            std::list<ImageComponents> prefComps;
            ImageBitDepthEnum prefDepth;
            getPreferredDepthAndComponents(-1, &prefComps, &prefDepth);
            for (std::list<ImageComponents>::iterator it = prefComps.begin(); it!=prefComps.end(); ++it) {
                compVec.push_back(*it);
            }
            comps->insert(std::make_pair(-1, compVec));
        }
        
        int maxInput = getMaxInputCount();
        for (int i = 0; i < maxInput; ++i) {
            EffectInstance* input = getInput(i);
            std::vector<ImageComponents> compVec;
            
            if (input) {
                std::list<ImageComponents> prefComps;
                ImageBitDepthEnum prefDepth;
                input->getPreferredDepthAndComponents(-1, &prefComps, &prefDepth);
                for (std::list<ImageComponents>::iterator it = prefComps.begin(); it!=prefComps.end(); ++it) {
                    compVec.push_back(*it);
                }
            }
            comps->insert(std::make_pair(i, compVec));

        }
    }
}

int
EffectInstance::getMaskChannel(int inputNb) const
{
    return getNode()->getMaskChannel(inputNb);
}

bool
EffectInstance::isMaskEnabled(int inputNb) const
{
    return getNode()->isMaskEnabled(inputNb);
}

void
EffectInstance::onKnobValueChanged(KnobI* /*k*/,
                                   Natron::ValueChangedReasonEnum /*reason*/,
                                   SequenceTime /*time*/,
                                   bool /*originatedFromMainThread*/)
{
}

int
EffectInstance::getThreadLocalRenderTime() const
{
    
    if (_imp->renderArgs.hasLocalData()) {
        const RenderArgs& args = _imp->renderArgs.localData();
        if (args._validArgs) {
            return args._time;
        }
    }
    
    if (_imp->frameRenderArgs.hasLocalData()) {
        const ParallelRenderArgs& args = _imp->frameRenderArgs.localData();
        if (args.validArgs) {
            return args.time;
        }
    }
    return getApp()->getTimeLine()->currentFrame();
}

bool
EffectInstance::getThreadLocalRenderedPlanes(std::map<Natron::ImageComponents,PlaneToRender> *outputPlanes,RectI* renderWindow) const
{
    if (_imp->renderArgs.hasLocalData()) {
        const RenderArgs& args = _imp->renderArgs.localData();
        if (args._validArgs) {
            assert(!args._outputPlanes.empty());
            *outputPlanes = args._outputPlanes;
            *renderWindow = args._renderWindowPixel;
            return true;
        }
    }
    return false;
}

void
EffectInstance::updateThreadLocalRenderTime(int time)
{
    if (QThread::currentThread() != qApp->thread() && _imp->renderArgs.hasLocalData()) {
         RenderArgs& args = _imp->renderArgs.localData();
        if (args._validArgs) {
            args._time = time;
        }
    }
}

void
EffectInstance::Implementation::runChangedParamCallback(KnobI* k,bool userEdited,const std::string& callback)
{
    std::vector<std::string> args;
    std::string error;
    Natron::getFunctionArguments(callback, &error, &args);
    if (!error.empty()) {
        _publicInterface->getApp()->appendToScriptEditor("Failed to run onParamChanged callback: " + error);
        return;
    }
    
    std::string signatureError;
    signatureError.append("The param changed callback supports the following signature(s):\n");
    signatureError.append("- callback(thisParam,thisNode,thisGroup,app,userEdited)");
    if (args.size() != 5) {
        _publicInterface->getApp()->appendToScriptEditor("Failed to run onParamChanged callback: " + signatureError);
        return;
    }
    
    if ((args[0] != "thisParam" || args[1] != "thisNode" || args[2] != "thisGroup" || args[3] != "app" || args[4] != "userEdited")) {
        _publicInterface->getApp()->appendToScriptEditor("Failed to run onParamChanged callback: " + signatureError);
        return;
    }
    
    std::string appID = _publicInterface->getApp()->getAppIDString();
    
    assert(k);
    std::string thisNodeVar = appID + ".";
    thisNodeVar.append(_publicInterface->getNode()->getFullyQualifiedName());
    
    boost::shared_ptr<NodeCollection> collection = _publicInterface->getNode()->getGroup();
    assert(collection);
    if (!collection) {
        return;
    }
    
    std::string thisGroupVar;
    NodeGroup* isParentGrp = dynamic_cast<NodeGroup*>(collection.get());
    if (isParentGrp) {
        thisGroupVar = appID + "." + isParentGrp->getNode()->getFullyQualifiedName();
    } else {
        thisGroupVar = appID;
    }

    
    std::stringstream ss;
    ss << callback << "(" << thisNodeVar << "." << k->getName() << "," << thisNodeVar << "," << thisGroupVar << "," << appID
    << ",";
    if (userEdited) {
        ss << "True";
    } else {
        ss << "False";
    }
    ss << ")\n";
   
    std::string script = ss.str();
    std::string err;
    std::string output;
    if (!Natron::interpretPythonScript(script, &err,&output)) {
        _publicInterface->getApp()->appendToScriptEditor(QObject::tr("Failed to execute callback: ").toStdString() + err);
    } else {
        if (!output.empty()) {
            _publicInterface->getApp()->appendToScriptEditor(output);
        }
    }
}

void
EffectInstance::onKnobValueChanged_public(KnobI* k,
                                          Natron::ValueChangedReasonEnum reason,
                                          SequenceTime time,
                                          bool originatedFromMainThread)
{

    NodePtr node = getNode();
//    if (!node->isNodeCreated()) {
//        return;
//    }

    if (isReader() && k->getName() == kOfxImageEffectFileParamName) {
        node->computeFrameRangeForReader(k);
    }

    
    KnobHelper* kh = dynamic_cast<KnobHelper*>(k);
    assert(kh);
    if ( kh && kh->isDeclaredByPlugin() ) {
        ////We set the thread storage render args so that if the instance changed action
        ////tries to call getImage it can render with good parameters.
        
        
        ParallelRenderArgsSetter frameRenderArgs(getApp()->getProject().get(),
                                                 time,
                                                 0, /*view*/
                                                 true,
                                                 false,
                                                 false,
                                                 0,
                                                 0,
                                                 0, //texture index
                                                 getApp()->getTimeLine().get());

        RECURSIVE_ACTION();
        knobChanged(k, reason, /*view*/ 0, time, originatedFromMainThread);
    }
    
    node->onEffectKnobValueChanged(k, reason);
    
    ///If there's a knobChanged Python callback, run it
    std::string pythonCB = getNode()->getKnobChangedCallback();
    
    if (!pythonCB.empty()) {
        bool userEdited = reason == eValueChangedReasonNatronGuiEdited ||
        reason == eValueChangedReasonUserEdited;
        _imp->runChangedParamCallback(k,userEdited,pythonCB);
    }

    
    ///Clear input images pointers that were stored in getImage() for the main-thread.
    ///This is safe to do so because if this is called while in render() it won't clear the input images
    ///pointers for the render thread. This is helpful for analysis effects which call getImage() on the main-thread
    ///and whose render() function is never called.
    _imp->clearInputImagePointers();
    
} // onKnobValueChanged_public

void
EffectInstance::clearLastRenderedImage()
{
    {
        QMutexLocker l(&_imp->lastRenderArgsMutex);
        _imp->lastPlanesRendered.clear();
    }
}

void
EffectInstance::aboutToRestoreDefaultValues()
{
    ///Invalidate the cache by incrementing the age
    NodePtr node = getNode();
    node->incrementKnobsAge();

    if ( node->areKeyframesVisibleOnTimeline() ) {
        node->hideKeyframesFromTimeline(true);
    }
}

/**
 * @brief Returns a pointer to the first non disabled upstream node.
 * When cycling through the tree, we prefer non optional inputs and we span inputs
 * from last to first.
 **/
Natron::EffectInstance*
EffectInstance::getNearestNonDisabled() const
{
    NodePtr node = getNode();
    if ( !node->isNodeDisabled() ) {
        return node->getLiveInstance();
    } else {
        ///Test all inputs recursively, going from last to first, preferring non optional inputs.
        std::list<Natron::EffectInstance*> nonOptionalInputs;
        std::list<Natron::EffectInstance*> optionalInputs;
        int maxInp = getMaxInputCount();

        ///We cycle in reverse by default. It should be a setting of the application.
        ///In this case it will return input B instead of input A of a merge for example.
        for (int i = 0; i < maxInp; ++i) {
            Natron::EffectInstance* inp = getInput(i);
            bool optional = isInputOptional(i);
            if (inp) {
                if (optional) {
                    optionalInputs.push_back(inp);
                } else {
                    nonOptionalInputs.push_back(inp);
                }
            }
        }

        ///Cycle through all non optional inputs first
        for (std::list<Natron::EffectInstance*> ::iterator it = nonOptionalInputs.begin(); it != nonOptionalInputs.end(); ++it) {
            Natron::EffectInstance* inputRet = (*it)->getNearestNonDisabled();
            if (inputRet) {
                return inputRet;
            }
        }

        ///Cycle through optional inputs...
        for (std::list<Natron::EffectInstance*> ::iterator it = optionalInputs.begin(); it != optionalInputs.end(); ++it) {
            Natron::EffectInstance* inputRet = (*it)->getNearestNonDisabled();
            if (inputRet) {
                return inputRet;
            }
        }

        ///We didn't find anything upstream, return
        return NULL;
    }
}

Natron::EffectInstance*
EffectInstance::getNearestNonDisabledPrevious(int* inputNb)
{
    assert(getNode()->isNodeDisabled());
    
    ///Test all inputs recursively, going from last to first, preferring non optional inputs.
    std::list<Natron::EffectInstance*> nonOptionalInputs;
    std::list<Natron::EffectInstance*> optionalInputs;
    int maxInp = getMaxInputCount();
    
    
    int localPreferredInput = -1;
    
    ///We cycle in reverse by default. It should be a setting of the application.
    ///In this case it will return input B instead of input A of a merge for example.
    for (int i = 0; i < maxInp; ++i) {
        Natron::EffectInstance* inp = getInput(i);
        bool optional = isInputOptional(i);
        if (inp) {
            if (optional) {
                if (localPreferredInput == -1) {
                    localPreferredInput = i;
                }
                optionalInputs.push_back(inp);
            } else {
                if (localPreferredInput == -1) {
                    localPreferredInput = i;
                }
                nonOptionalInputs.push_back(inp);
            }
        }
    }
    
    
    ///Cycle through all non optional inputs first
    for (std::list<Natron::EffectInstance*> ::iterator it = nonOptionalInputs.begin(); it != nonOptionalInputs.end(); ++it) {
        if ((*it)->getNode()->isNodeDisabled()) {
            Natron::EffectInstance* inputRet = (*it)->getNearestNonDisabledPrevious(inputNb);
            if (inputRet) {
                return inputRet;
            }
        }
    }
    
    ///Cycle through optional inputs...
    for (std::list<Natron::EffectInstance*> ::iterator it = optionalInputs.begin(); it != optionalInputs.end(); ++it) {
        if ((*it)->getNode()->isNodeDisabled()) {
            Natron::EffectInstance* inputRet = (*it)->getNearestNonDisabledPrevious(inputNb);
            if (inputRet) {
                return inputRet;
            }
        }
    }
    
    *inputNb = localPreferredInput;
    return this;
    
}

Natron::EffectInstance*
EffectInstance::getNearestNonIdentity(int time)
{
    
    U64 hash = getHash();
    RenderScale scale;
    scale.x = scale.y = 1.;
    
    RectD rod;
    bool isProjectFormat;
    Natron::StatusEnum stat = getRegionOfDefinition_public(hash, time, scale, 0, &rod, &isProjectFormat);
    
    double par = getPreferredAspectRatio();
    
    ///Ignore the result of getRoD if it failed
    (void)stat;
    
    SequenceTime inputTimeIdentity;
    int inputNbIdentity;
    
    if ( !isIdentity_public(hash, time, scale, rod, par, 0, &inputTimeIdentity, &inputNbIdentity) ) {
        return this;
    } else {
        
        if (inputNbIdentity < 0) {
            return this;
        }
        Natron::EffectInstance* effect = getInput(inputNbIdentity);
        return effect ? effect->getNearestNonIdentity(time) : this;
    }

}

void
EffectInstance::restoreClipPreferences()
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

void
EffectInstance::onNodeHashChanged(U64 hash)
{
    
    ///Always running in the MAIN THREAD
    assert(QThread::currentThread() == qApp->thread());
    
    ///Invalidate actions cache
    _imp->actionsCache.invalidateAll(hash);
    
    const std::vector<boost::shared_ptr<KnobI> >& knobs = getKnobs();
    for (std::vector<boost::shared_ptr<KnobI> >::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        for (int i = 0; i < (*it)->getDimension(); ++i) {
            (*it)->clearExpressionsResults(i);
        }
    }
}

bool
EffectInstance::canSetValue() const
{
    return !getNode()->isNodeRendering() || appPTR->isBackground();
}

SequenceTime
EffectInstance::getCurrentTime() const
{
    return getThreadLocalRenderTime();
}

int
EffectInstance::getCurrentView() const
{
    if (_imp->renderArgs.hasLocalData()) {
        const RenderArgs& args = _imp->renderArgs.localData();
        if (args._validArgs) {
            return args._view;
        }
    }
    
    return 0;
}

SequenceTime
EffectInstance::getFrameRenderArgsCurrentTime() const
{
    if (_imp->frameRenderArgs.hasLocalData()) {
        const ParallelRenderArgs& args = _imp->frameRenderArgs.localData();
        if (args.validArgs) {
            return args.time;
        }
    }
    return getApp()->getTimeLine()->currentFrame();
}

int
EffectInstance::getFrameRenderArgsCurrentView() const
{
    if (_imp->frameRenderArgs.hasLocalData()) {
        const ParallelRenderArgs& args = _imp->frameRenderArgs.localData();
        if (args.validArgs) {
            return args.view;
        }
    }
    
    return 0;
}

#ifdef DEBUG
void
EffectInstance::checkCanSetValueAndWarn() const
{
    if (!checkCanSetValue()) {
        qDebug() << getScriptName_mt_safe().c_str() << ": setValue()/setValueAtTime() was called during an action that is not allowed to call this function.";
    }
}
#endif

static
void isFrameVaryingOrAnimated_impl(const Natron::EffectInstance* node,bool *ret)
{
    if (node->isFrameVarying() || node->getHasAnimation() || node->getNode()->getRotoContext()) {
        *ret = true;
    } else {
        int maxInputs = node->getMaxInputCount();
        for (int i = 0; i < maxInputs; ++i) {
            Natron::EffectInstance* input = node->getInput(i);
            if (input) {
                isFrameVaryingOrAnimated_impl(input,ret);
                if (*ret) {
                    return;
                }
            }
        }
    }
}

bool
EffectInstance::isFrameVaryingOrAnimated_Recursive() const
{
    bool ret = false;
    isFrameVaryingOrAnimated_impl(this,&ret);
    return ret;
}


OutputEffectInstance::OutputEffectInstance(boost::shared_ptr<Node> node)
    : Natron::EffectInstance(node)
      , _writerCurrentFrame(0)
      , _writerFirstFrame(0)
      , _writerLastFrame(0)
      , _outputEffectDataLock(new QMutex)
      , _renderController(0)
      , _engine(0)
{
}

OutputEffectInstance::~OutputEffectInstance()
{
    if (_engine) {
        ///Thread must have been killed before.
        assert( !_engine->hasThreadsAlive() );
    }
    delete _engine;
    delete _outputEffectDataLock;
}

void
OutputEffectInstance::renderCurrentFrame(bool canAbort)
{
    _engine->renderCurrentFrame(canAbort);
}

bool
OutputEffectInstance::ifInfiniteclipRectToProjectDefault(RectD* rod) const
{
    if ( !getApp()->getProject() ) {
        return false;
    }
    /*If the rod is infinite clip it to the project's default*/
    Format projectDefault;
    getRenderFormat(&projectDefault);
    // BE CAREFUL:
    // std::numeric_limits<int>::infinity() does not exist (check std::numeric_limits<int>::has_infinity)
    // an int can not be equal to (or compared to) std::numeric_limits<double>::infinity()
    bool isRodProjctFormat = false;
    if (rod->left() <= kOfxFlagInfiniteMin) {
        rod->set_left( projectDefault.left() );
        isRodProjctFormat = true;
    }
    if (rod->bottom() <= kOfxFlagInfiniteMin) {
        rod->set_bottom( projectDefault.bottom() );
        isRodProjctFormat = true;
    }
    if (rod->right() >= kOfxFlagInfiniteMax) {
        rod->set_right( projectDefault.right() );
        isRodProjctFormat = true;
    }
    if (rod->top() >= kOfxFlagInfiniteMax) {
        rod->set_top( projectDefault.top() );
        isRodProjctFormat = true;
    }

    return isRodProjctFormat;
}

void
OutputEffectInstance::renderFullSequence(BlockingBackgroundRender* renderController,int first,int last)
{
    _renderController = renderController;
    
    ///Make sure that the file path exists
    boost::shared_ptr<KnobI> fileParam = getKnobByName(kOfxImageEffectFileParamName);
    if (fileParam) {
        Knob<std::string>* isString = dynamic_cast<Knob<std::string>*>(fileParam.get());
        if (isString) {
            std::string pattern = isString->getValue();
            std::string path = SequenceParsing::removePath(pattern);
            std::map<std::string,std::string> env;
            getApp()->getProject()->getEnvironmentVariables(env);
            Project::expandVariable(env, path);
            QDir().mkpath(path.c_str());
        }
    }
    ///If you want writers to render backward (from last to first), just change the flag in parameter here
    _engine->renderFrameRange(first,last,OutputSchedulerThread::eRenderDirectionForward);

}

void
OutputEffectInstance::notifyRenderFinished()
{
    if (_renderController) {
        _renderController->notifyFinished();
        _renderController = 0;
    }
}

int
OutputEffectInstance::getCurrentFrame() const
{
    QMutexLocker l(_outputEffectDataLock);

    return _writerCurrentFrame;
}

void
OutputEffectInstance::setCurrentFrame(int f)
{
    QMutexLocker l(_outputEffectDataLock);

    _writerCurrentFrame = f;
}

void
OutputEffectInstance::incrementCurrentFrame()
{
    QMutexLocker l(_outputEffectDataLock);
    ++_writerCurrentFrame;
}

void
OutputEffectInstance::decrementCurrentFrame()
{
    QMutexLocker l(_outputEffectDataLock);
    --_writerCurrentFrame;
}

int
OutputEffectInstance::getFirstFrame() const
{
    QMutexLocker l(_outputEffectDataLock);

    return _writerFirstFrame;
}

void
OutputEffectInstance::setFirstFrame(int f)
{
    QMutexLocker l(_outputEffectDataLock);

    _writerFirstFrame = f;
}

int
OutputEffectInstance::getLastFrame() const
{
    QMutexLocker l(_outputEffectDataLock);

    return _writerLastFrame;
}

void
OutputEffectInstance::setLastFrame(int f)
{
    QMutexLocker l(_outputEffectDataLock);

    _writerLastFrame = f;
}

void
OutputEffectInstance::initializeData()
{
    _engine= createRenderEngine();
}

RenderEngine*
OutputEffectInstance::createRenderEngine()
{
    return new RenderEngine(this);
}

double
EffectInstance::getPreferredFrameRate() const
{
    return getApp()->getProjectFrameRate();
}

void
EffectInstance::checkOFXClipPreferences_recursive(double time,
                                       const RenderScale & scale,
                                       const std::string & reason,
                                       bool forceGetClipPrefAction,
                                       std::list<Natron::Node*>& markedNodes)
{
    NodePtr node = getNode();
    std::list<Natron::Node*>::iterator found = std::find(markedNodes.begin(), markedNodes.end(), node.get());
    if (found != markedNodes.end()) {
        return;
    }
    
    
    checkOFXClipPreferences(time, scale, reason, forceGetClipPrefAction);
    markedNodes.push_back(node.get());
    
    std::list<Natron::Node*>  outputs;
    node->getOutputsWithGroupRedirection(outputs);
    for (std::list<Natron::Node*>::const_iterator it = outputs.begin(); it != outputs.end(); ++it) {
        (*it)->getLiveInstance()->checkOFXClipPreferences_recursive(time, scale, reason, forceGetClipPrefAction,markedNodes);
    }
}

void
EffectInstance::checkOFXClipPreferences_public(double time,
                                    const RenderScale & scale,
                                    const std::string & reason,
                                    bool forceGetClipPrefAction,
                                    bool recurse)
{
    assert(QThread::currentThread() == qApp->thread());
    
    if (recurse) {
        std::list<Natron::Node*> markedNodes;
        checkOFXClipPreferences_recursive(time, scale, reason, forceGetClipPrefAction, markedNodes);
    } else {
        checkOFXClipPreferences(time, scale, reason, forceGetClipPrefAction);
    }
}


