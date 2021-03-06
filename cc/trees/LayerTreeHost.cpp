
#include "cc/trees/LayerTreeHost.h"

#include "SkCanvas.h"
#include "cc/blink/WebLayerImpl.h"
#include "cc/blink/WebLayerImplClient.h"
#include "cc/trees/DrawProperties.h"
#include "cc/trees/LayerSorter.h"
#include "cc/trees/ActionsFrameGroup.h"
#include "cc/tiles/Tile.h"
#include "cc/tiles/TileGrid.h"
#include "cc/raster/RasterTaskWorkerThreadPool.h"
#include "cc/layers/CompositingLayer.h"
#include "cc/playback/LayerChangeAction.h"

#include "third_party/WebKit/public/web/WebViewClient.h"
#include "third_party/WebKit/public/platform/WebFloatSize.h"
#include "third_party/WebKit/public/platform/WebGestureCurveTarget.h"
#include "third_party/WebKit/Source/platform/graphics/GraphicsContext.h" // TODO

using namespace blink;

namespace cc {

LayerTreeHost* gLayerTreeHost = nullptr;

LayerTreeHost::LayerTreeHost(blink::WebViewClient* webViewClient)
{
    m_rootLayer = nullptr;
    m_deviceScaleFactor = 1.0f;
    m_backgroundColor = 0xff00ffff;
    m_hasTransparentBackground = false;
    m_visible = true;
    m_pageScaleFactor = 1.0f;
    m_minimum = 1.0f;
    m_maximum = 1.0f;
    m_webViewClient = webViewClient;
    m_needsFullTreeSync = true;
    m_needTileRender = true;
    m_3dNodesCount = 0;
    m_tilesMutex = new WTF::Mutex();
    m_rasterNotifMutex = new WTF::Mutex();
    m_newestDrawingIndex = 0;
    m_drawingIndex = 1;
// 	m_newestActionId = 0;
//  m_curActionId = 1;
    m_actionsFrameGroup = new ActionsFrameGroup(this);
    m_isDestroying = false;

    gLayerTreeHost = this;
}

extern WTF::Vector<LayerChangeAction*>* gTestActions;

LayerTreeHost::~LayerTreeHost()
{
    m_isDestroying = true;

    while (0 != RasterTaskWorkerThreadPool::shared()->pendingRasterTaskNum()) { ::Sleep(20); }
    applyActions(true);

	for (WTF::HashMap<int, cc_blink::WebLayerImpl*>::iterator it = m_liveLayers.begin(); m_liveLayers.end() != it; ++it) {
        cc_blink::WebLayerImpl* layer = it->value;
        layer->setLayerTreeHost(nullptr);
	}
    m_liveLayers.clear();

    for (WTF::HashMap<int, CompositingLayer*>::iterator it = m_liveCCLayers.begin(); m_liveCCLayers.end() != it; ++it) {
        CompositingLayer* ccLayer = it->value;
        ccLayer->setParent(nullptr);
        delete ccLayer;
    }
    m_liveCCLayers.clear();
    
	for (size_t i = 0; i < m_dirtyLayersGroup.size(); ++i) {
		DirtyLayers* dirtyLayers = m_dirtyLayersGroup[i];
		delete dirtyLayers;
	}
	m_dirtyLayersGroup.clear();
	deleteTilesToUIThreadRelease();

	delete m_rasterNotifMutex;
	m_rasterNotifMutex = nullptr;

    delete m_tilesMutex;
    m_tilesMutex = nullptr;

    delete m_actionsFrameGroup;
    m_actionsFrameGroup = nullptr;
}

bool LayerTreeHost::isDestroying() const
{
    return m_isDestroying;
}

void LayerTreeHost::registerLayer(cc_blink::WebLayerImpl* layer)
{
    m_liveLayers.add(layer->id(), layer);
}

void LayerTreeHost::unregisterLayer(cc_blink::WebLayerImpl* layer)
{
    m_liveLayers.remove(layer->id());
}

void LayerTreeHost::registerCCLayer(CompositingLayer* layer)
{
    m_liveCCLayers.add(layer->id(), layer);
}

void LayerTreeHost::unregisterCCLayer(CompositingLayer* layer)
{
    m_liveCCLayers.remove(layer->id());
}

cc_blink::WebLayerImpl* LayerTreeHost::getLayerById(int id)
{
    WTF::HashMap<int, cc_blink::WebLayerImpl*>::iterator it = m_liveLayers.find(id);
    if (m_liveLayers.end() != it)
        return it->value;

    return nullptr;
}

CompositingLayer* LayerTreeHost::getCCLayerById(int id)
{
    WTF::HashMap<int, CompositingLayer*>::iterator it = m_liveCCLayers.find(id);
    if (m_liveCCLayers.end() != it)
        return it->value;

    return nullptr;
}

void LayerTreeHost::increaseNodesCount()
{
    ++m_3dNodesCount;
}

void LayerTreeHost::decreaseNodesCount()
{
    --m_3dNodesCount;
}

bool LayerTreeHost::has3dNodes()
{
    return m_3dNodesCount > 0;
}

WTF::Mutex* LayerTreeHost::tilesMutex()
{
    return m_tilesMutex;
}

int64 LayerTreeHost::createDrawingIndex()
{
    ++m_newestDrawingIndex;
    return m_newestDrawingIndex;
}

int64 LayerTreeHost::drawingIndex()
{
    return m_drawingIndex;
}

void LayerTreeHost::addRasteringIndex(int64 index)
{
    WTF::MutexLocker locker(*m_tilesMutex);
    m_rasteringIndexs.append(index);
}

int64 LayerTreeHost::frontRasteringIndex()
{
    WTF::MutexLocker locker(*m_tilesMutex);
    ASSERT(0 != m_rasteringIndexs.size());
    if (0 == m_rasteringIndexs.size())
        return -1;
    return m_rasteringIndexs[0];
}

void LayerTreeHost::popRasteringIndex()
{
    WTF::MutexLocker locker(*m_tilesMutex);
    if (0 < m_rasteringIndexs.size())
        m_rasteringIndexs.remove(0);
}

void LayerTreeHost::setWebGestureCurveTarget(blink::WebGestureCurveTarget* webGestureCurveTarget)
{
    m_webGestureCurveTarget = webGestureCurveTarget;
}

void LayerTreeHost::setNeedsCommit()
{
    // 由光栅化线程来提起脏区域，所以这里直接指定需要开始下一帧，光删化完毕后由光栅化线程通过requestRepaint发起重绘
    m_webViewClient->scheduleAnimation();
}

void LayerTreeHost::setNeedsFullTreeSync()
{
    m_needsFullTreeSync = true;
    m_webViewClient->scheduleAnimation();
}

void LayerTreeHost::requestRepaint(const blink::IntRect& repaintRect)
{
    m_webViewClient->didInvalidateRect(blink::WebRect(repaintRect));
}

void LayerTreeHost::requestDrawFrameLocked(DirtyLayers* dirtyLayers, Vector<Tile*>* tilesToUIThreadRelease)
{
    m_rasterNotifMutex->lock();
    // 等待，必须按index的顺序;
    m_dirtyLayersGroup.append(dirtyLayers);
	m_tilesToUIThreadRelease.appendVector(*tilesToUIThreadRelease);
	delete tilesToUIThreadRelease;
    m_rasterNotifMutex->unlock();
    setNeedsCommit();
}

static bool compareDirtyLayer(DirtyLayers*& left, DirtyLayers*& right)
{
    return left->drawingIndex() < right->drawingIndex();
}

static bool compareAction(LayerChangeAction*& left, LayerChangeAction*& right)
{
	return left->actionId() < right->actionId();
}

void LayerTreeHost::beginRecordActions()
{
    m_actionsFrameGroup->beginRecordActions();
}

void LayerTreeHost::endRecordActions()
{
    m_actionsFrameGroup->endRecordActions();
}

void LayerTreeHost::appendLayerChangeAction(LayerChangeAction* action)
{
	m_actionsFrameGroup->saveLayerChangeAction(action);

	setNeedsAnimate();

// 	String outString = String::format("LayerTreeHost::appendLayerChangeAction: %d %d \n", (int)(action->type()), (int)action->actionId());
// 	OutputDebugStringW(outString.charactersWithNullTermination().data());
}

int64 LayerTreeHost::genActionId()
{
    return m_actionsFrameGroup->genActionId();
}

bool LayerTreeHost::preDrawFrame()
{
	return applyActions(false);
}

bool LayerTreeHost::applyActions(bool needCheck)
{
    return m_actionsFrameGroup->applyActions(needCheck);
}

void LayerTreeHost::deleteTilesToUIThreadRelease()
{
	m_rasterNotifMutex->lock();
	size_t size = m_tilesToUIThreadRelease.size();
	for (size_t i = 0; i < size; ++i) {
		cc::Tile* tile = m_tilesToUIThreadRelease[i];
		tile->unref(FROM_HERE);
	}
	m_tilesToUIThreadRelease.clear();
	m_rasterNotifMutex->unlock();
}

void LayerTreeHost::postDrawFrame()
{
	deleteTilesToUIThreadRelease();
}

void LayerTreeHost::releaseTilesFromRasterThread(const TileGrid* tileGrid)
{
    m_rasterNotifMutex->lock();
	for (size_t i = 0; i < m_tilesToUIThreadRelease.size(); ++i) {
		cc::Tile* tile = m_tilesToUIThreadRelease[i];
		if (!tile->isSameTileGrid(tileGrid))
			continue;
		
		tile->unref(FROM_HERE);
		m_tilesToUIThreadRelease.remove(i);
		--i;
	}
    m_rasterNotifMutex->unlock();
}

void LayerTreeHost::scrollBy(const blink::WebFloatSize& delta, const blink::WebFloatSize& velocity)
{

}

blink::IntSize LayerTreeHost::canDrawSize()
{
    blink::IntSize outSize = deviceViewportSize();
    outSize.scale(1.2);
    return outSize;
}

static blink::WebDoublePoint getEffectiveTotalScrollOffset(cc_blink::WebLayerImpl* layer) {
    //     if (layer->DrawIgnoreScrollOffset())
    //         return gfx::Vector2dF();
    blink::WebDoublePoint offset = layer->scrollPositionDouble();
    // The scroll parent's total scroll offset (scroll offset + scroll delta)
    // can't be used because its scroll offset has already been applied to the
    // scroll children's positions by the main thread layer positioning code.
    //     if (layer->scrollParent())
    //         offset += layer->scroll_parent()->ScrollDelta();
    return offset;
}

static void updateLayer(cc_blink::WebLayerImpl* layer, SkCanvas* canvas, const blink::IntRect& clip)
{
    blink::WebFloatPoint position = layer->position();
    blink::WebSize size = layer->bounds();
    blink::IntRect childClip(0, 0, size.width, size.height);

//     childClip.intersect(clip);
//     if (childClip.isEmpty())
//         return;

//     String out = String::format("LayerTreeHost::updateLayer: %p\n", layer);
//     OutputDebugStringW(out.charactersWithNullTermination().data());

    layer->updataAndPaintContents(canvas, childClip);
}

static void updateLayerChildren(cc_blink::WebLayerImpl* layer, SkCanvas* canvas, const blink::IntRect& clip, bool needsFullTreeSync)
{
    blink::WebFloatPoint currentLayerPosition = layer->position();
    blink::WebDoublePoint effectiveTotalScrollOffset = getEffectiveTotalScrollOffset(layer);
    blink::WebFloatPoint currentLayerPositionScrolled(currentLayerPosition.x - effectiveTotalScrollOffset.x, currentLayerPosition.y - effectiveTotalScrollOffset.y);

    blink::WebFloatPoint3D transformOrigin = layer->transformOrigin();

    SkMatrix44 combinedTransform(SkMatrix44::kIdentity_Constructor);
    combinedTransform.preTranslate(currentLayerPositionScrolled.x + transformOrigin.x, currentLayerPositionScrolled.y + transformOrigin.y, currentLayerPosition.x);
    combinedTransform.preConcat(layer->transform());
    combinedTransform.preTranslate(-transformOrigin.x, -transformOrigin.y, -transformOrigin.z);

    if (!combinedTransform.isIdentity()) {
        canvas->save();
        canvas->concat(combinedTransform);
    }

    blink::WebSize size = layer->bounds();
    blink::IntRect layerRect(0, 0, size.width, size.height);

    bool layerClip = 0 != layerRect.width() && 0 != layerRect.height() && layer->masksToBounds();
    if (layerClip) {
        canvas->save();

//         OwnPtr<blink::GraphicsContext> context = blink::GraphicsContext::deprecatedCreateWithCanvas(canvas, blink::GraphicsContext::NothingDisabled);
//         if (childClip.height() == 37) {
//             context->setStrokeStyle(blink::SolidStroke);
//             context->setStrokeColor(0xff000000 | (::GetTickCount() + base::RandInt(0, 0x1223345)));
//             context->drawLine(blink::IntPoint(layerRect.x(), layerRect.y()), blink::IntPoint(layerRect.maxX(), layerRect.maxY()));
//             context->drawLine(blink::IntPoint(layerRect.maxX(), layerRect.y()), blink::IntPoint(layerRect.x(), layerRect.maxY()));
//             context->strokeRect(layerRect, 1);
//         }

        canvas->clipRect(layerRect, SkRegion::kIntersect_Op, false);
    }

    if (
#if 0
        layer->dirty() ||
#endif
        needsFullTreeSync)
        updateLayer(layer, canvas, clip);

    blink::IntRect clipInChildCoordinate = clip;
    clipInChildCoordinate.move(-currentLayerPositionScrolled.x, -currentLayerPositionScrolled.y);

    WTF::Vector<cc_blink::WebLayerImpl*>& children = layer->children();
    for (size_t i = 0; i < children.size(); ++i) {
        cc_blink::WebLayerImpl* child = children[i];
#if 1
        if (!(child->dirty() || child->childrenDirty() || needsFullTreeSync))
            continue;
#endif

        child->clearChildrenDirty();
        updateLayerChildren(child, canvas, clipInChildCoordinate, needsFullTreeSync);
    }

    if (layerClip)
        canvas->restore();

    if (!combinedTransform.isIdentity())
        canvas->restore();
}

void LayerTreeHost::updateLayers(SkCanvas* canvas, const blink::IntRect& clip, bool needsFullTreeSync)
{
    if (!m_rootLayer)
        return;

    updateLayerChildren(m_rootLayer, canvas, clip, m_needsFullTreeSync || needsFullTreeSync);
    m_needsFullTreeSync = false;
}

static void flattenTo2d(SkMatrix44& matrix)
{
    matrix.set(2, 0, 0.0);
    matrix.set(2, 1, 0.0);
    matrix.set(0, 2, 0.0);
    matrix.set(1, 2, 0.0);
    matrix.set(2, 2, 1.0);
    matrix.set(3, 2, 0.0);
    matrix.set(2, 3, 0.0);
}

void LayerTreeHost::recordDraw()
{
    if (!m_rootLayer)
        return;

	updateLayersDrawProperties();

    cc::RasterTaskGroup* taskGroup = cc::RasterTaskWorkerThreadPool::shared()->beginPostRasterTask(this);
    m_rootLayer->recordDrawChildren(taskGroup, 0);
    taskGroup->endPostRasterTask();

    m_needsFullTreeSync = false;
}

void printTrans(const SkMatrix44& transform, int deep)
{
    SkMScalar total = 0.0;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            total += transform.get(i, j);
        }
    }
    
    String outString = String::format("LayerTreeHost::printTrans:%d, %f \n", deep, total);
    OutputDebugStringW(outString.charactersWithNullTermination().data());
}

void LayerTreeHost::drawToCanvas(SkCanvas* canvas, const IntRect& clip)
{
    if (!m_rootCCLayer)
        return;

    SkPaint clearColorPaint;
    clearColorPaint.setColor(0xffffffff | m_backgroundColor); // weolar
    //clearColorPaint.setColor(0xfff0504a);

    // http://blog.csdn.net/to_be_designer/article/details/48530921
    clearColorPaint.setXfermodeMode(SkXfermode::kSrcOver_Mode); // SkXfermode::kSrcOver_Mode
    canvas->drawRect((SkRect)clip, clearColorPaint);

    m_rootCCLayer->drawToCanvasChildren(this, canvas, clip, 0);
}

struct DrawPropertiesFromAncestor {
	DrawPropertiesFromAncestor() 
	{
	    transform = SkMatrix44(SkMatrix44::kIdentity_Constructor);
		opacity = 1.0;
	}

	SkMatrix44 transform;
	float opacity;
};

static void updateChildLayersDrawProperties(cc_blink::WebLayerImpl* layer, LayerSorter& layerSorter,  const DrawPropertiesFromAncestor& propFromAncestor, int deep)
{
    WTF::Vector<cc_blink::WebLayerImpl*>& children = layer->children();
    
    for (size_t i = 0; i < children.size(); ++i) {
        cc_blink::WebLayerImpl* child = children[i];
        DrawProperties* drawProperties = child->drawProperties();

        blink::WebFloatPoint currentLayerPosition = child->position();
        blink::WebDoublePoint effectiveTotalScrollOffset = getEffectiveTotalScrollOffset(child);
        blink::WebFloatPoint currentLayerPositionScrolled(currentLayerPosition.x - effectiveTotalScrollOffset.x, currentLayerPosition.y - effectiveTotalScrollOffset.y);
        blink::WebFloatPoint3D transformOrigin = child->transformOrigin();

        SkMatrix44 currentTransform(SkMatrix44::kIdentity_Constructor);
        SkMatrix44 childTransform = child->transform();

        if (!childTransform.isIdentity()) {
            currentTransform.preTranslate(currentLayerPositionScrolled.x + transformOrigin.x, currentLayerPositionScrolled.y + transformOrigin.y, transformOrigin.z);
            currentTransform.preConcat(childTransform);
            currentTransform.preTranslate(-transformOrigin.x, -transformOrigin.y, -transformOrigin.z);
        } else {
            currentTransform.preTranslate(currentLayerPositionScrolled.x, currentLayerPositionScrolled.y, 0);
        }

        // Flatten to 2D if the layer doesn't preserve 3D.
        SkMatrix44 combinedTransform = propFromAncestor.transform;
        combinedTransform.preConcat(currentTransform);

        SkMatrix44 transformToAncestorIfFlatten = combinedTransform;
        if (child->shouldFlattenTransform())
            flattenTo2d(transformToAncestorIfFlatten);

        drawProperties->screenSpaceTransform = combinedTransform;
        drawProperties->targetSpaceTransform = combinedTransform;
        drawProperties->currentTransform = currentTransform;
		//drawProperties->opacity = propFromAncestor.opacity;

		DrawPropertiesFromAncestor prop;
		prop.transform = transformToAncestorIfFlatten;
		prop.opacity *= child->opacity();
        updateChildLayersDrawProperties(child, layerSorter, prop, deep + 1);
    }

    if (children.size() && layer->layerTreeHost()->has3dNodes() && layer->is3dSorted()) {
        layerSorter.Sort(children.begin(), children.end());
    }
}

void LayerTreeHost::updateLayersDrawProperties()
{
    LayerSorter layerSorter;
    SkMatrix44 transform(SkMatrix44::kIdentity_Constructor);

	DrawPropertiesFromAncestor prop;
    updateChildLayersDrawProperties(m_rootLayer, layerSorter, prop, 0);
}

static void showDebugChildren(cc_blink::WebLayerImpl* layer, int deep)
{
    Vector<LChar> blankSpaceString;
    blankSpaceString.resize(deep);
    blankSpaceString.fill(' ');

    WTF::Vector<cc_blink::WebLayerImpl*>& children = layer->children();
    for (size_t i = 0; i < children.size(); ++i) {
        cc_blink::WebLayerImpl* child = children[i];

        blink::WebFloatPoint position = child->position();
        blink::WebSize bounds = child->bounds();
        
        String msg = String::format("%p, %d %d %d %d - %d, %d %d %d, %f\n", child,
            (int)position.x, (int)position.y, bounds.width, bounds.height, child->id(), child->drawsContent(), child->masksToBounds(), child->opaque(), child->opacity());
        msg.insert(blankSpaceString.data(), blankSpaceString.size(), 0);
        OutputDebugStringA(msg.utf8().data());

        showDebugChildren(child, deep + 1);
    }
}

void LayerTreeHost::showDebug()
{
    if (!m_rootLayer)
        return;
    showDebugChildren(m_rootLayer, 1);
}

// Sets the root of the tree. The root is set by way of the constructor.
void LayerTreeHost::setRootLayer(const blink::WebLayer& layer)
{
    m_rootLayer = (cc_blink::WebLayerImpl*)&layer;
    m_rootLayer->setLayerTreeHost(this);

	applyActions(false);

	m_rootCCLayer = getCCLayerById(m_rootLayer->id());

    setNeedsFullTreeSync();
}

void LayerTreeHost::clearRootLayer()
{
    m_rootLayer = nullptr;

	while (0 != RasterTaskWorkerThreadPool::shared()->pendingRasterTaskNum()) {	::Sleep(20); }
	applyActions(false);

	m_rootCCLayer = nullptr;
}

void LayerTreeHost::setViewportSize(const blink::WebSize& deviceViewportSize)
{
    m_deviceViewportSize = deviceViewportSize;
}

// Gives the viewport size in physical device pixels.
blink::WebSize LayerTreeHost::deviceViewportSize() const
{
    return blink::WebSize(m_deviceViewportSize.width(), m_deviceViewportSize.height());
}

void LayerTreeHost::setDeviceScaleFactor(float scale)
{
    m_deviceScaleFactor = scale;
}

float LayerTreeHost::deviceScaleFactor() const
{
    return m_deviceScaleFactor;
}

// Sets the background color for the viewport.
void LayerTreeHost::setBackgroundColor(blink::WebColor color)
{
    m_backgroundColor = color;
}

// Sets the background transparency for the viewport. The default is 'false'.
void LayerTreeHost::setHasTransparentBackground(bool b)
{
    m_hasTransparentBackground = b;
}

void LayerTreeHost::registerForAnimations(blink::WebLayer* layer)
{
	// 不能在这里设置LayerTreeHost，因为popup 类型的会把新窗口的layer调用本接口到老的host来。
// 	cc_blink::WebLayerImpl* layerImpl = (cc_blink::WebLayerImpl*)layer;
// 	layerImpl->setLayerTreeHost(this);
}

// Sets whether this view is visible. In threaded mode, a view that is not visible will not
// composite or trigger updateAnimations() or layout() calls until it becomes visible.
void LayerTreeHost::setVisible(bool visible)
{
    m_visible = visible;
}

// Sets the current page scale factor and minimum / maximum limits. Both limits are initially 1 (no page scale allowed).
void LayerTreeHost::setPageScaleFactorAndLimits(float pageScaleFactor, float minimum, float maximum)
{
    m_pageScaleFactor = pageScaleFactor;
    m_minimum = minimum;
    m_maximum = maximum;
}

// Starts an animation of the page scale to a target scale factor and scroll offset.
// If useAnchor is true, destination is a point on the screen that will remain fixed for the duration of the animation.
// If useAnchor is false, destination is the final top-left scroll position.
void LayerTreeHost::startPageScaleAnimation(const blink::WebPoint& destination, bool useAnchor, float newPageScale, double durationSec)
{

}

void LayerTreeHost::setNeedsAnimate()
{
    m_webViewClient->scheduleAnimation();
}

void LayerTreeHost::finishAllRendering()
{
    OutputDebugStringW(L"LayerTreeHost::finishAllRendering");
}

} // cc