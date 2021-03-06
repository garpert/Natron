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

#ifndef NATRON_GUI_NODEGRAPH_H_
#define NATRON_GUI_NODEGRAPH_H_

// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>

#include "Global/Macros.h"
CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QGraphicsView>
#include <QDialog>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#if !defined(Q_MOC_RUN) && !defined(SBK_RUN)
#include <boost/noncopyable.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>
#endif

#include "Engine/NodeGraphI.h"
#include "Engine/ScriptObject.h"
#include "Global/GlobalDefines.h"

class QVBoxLayout;
class QScrollArea;
class QEvent;
class QKeyEvent;
class Gui;
class NodeGui;
class QDropEvent;
class QUndoCommand;
class QDragEnterEvent;
class NodeSerialization;
class NodeGuiSerialization;
class NodeBackDropSerialization;
class NodeCollection;
class ViewerTab;
struct NodeGraphPrivate;
namespace Natron {
class Node;
}

class NodeGraph : public QGraphicsView, public NodeGraphI, public ScriptObject, public boost::noncopyable
{
    Q_OBJECT

public:

    explicit NodeGraph(Gui* gui,
                       const boost::shared_ptr<NodeCollection>& group,
                       QGraphicsScene* scene = 0,
                       QWidget *parent = 0);

    virtual ~NodeGraph();
    
    boost::shared_ptr<NodeCollection> getGroup() const;

    const std::list< boost::shared_ptr<NodeGui> > & getSelectedNodes() const;
    boost::shared_ptr<NodeGui> createNodeGUI(QVBoxLayout *dockContainer,const boost::shared_ptr<Natron::Node> & node,bool requestedByLoad,
                                             double xPosHint,double yPosHint,bool pushUndoRedoCommand,bool autoConnect);

    void selectNode(const boost::shared_ptr<NodeGui> & n,bool addToSelection);
    
    void setSelection(const std::list<boost::shared_ptr<NodeGui> >& nodes);
    
    void clearSelection();

    ///The visible portion of the graph, in scene coordinates.
    QRectF visibleSceneRect() const;
    QRect visibleWidgetRect() const;

    void deselect();

    QImage getFullSceneScreenShot();

    bool areAllNodesVisible();

    /**
     * @brief Repaint the navigator
     **/
    void updateNavigator();

    const std::list<boost::shared_ptr<NodeGui> > & getAllActiveNodes() const;
    std::list<boost::shared_ptr<NodeGui> > getAllActiveNodes_mt_safe() const;

    void moveToTrash(NodeGui* node);

    void restoreFromTrash(NodeGui* node);

    QGraphicsItem* getRootItem() const;
    Gui* getGui() const;

    void discardGuiPointer();
    void discardScenePointer();

    void refreshAllEdges();

    /**
     * @brief Removes the given node from the nodegraph, using the undo/redo stack.
     **/
    void removeNode(const boost::shared_ptr<NodeGui> & node);

    void centerOnItem(QGraphicsItem* item);

    void setUndoRedoStackLimit(int limit);

    void deleteNodepluginsly(boost::shared_ptr<NodeGui> n);

    std::list<boost::shared_ptr<NodeGui> > getNodesWithinBackDrop(const boost::shared_ptr<NodeGui>& node) const;

    void selectAllNodes(bool onlyInVisiblePortion);

    /**
     * @brief Calls setParentItem(NULL) on all items of the scene to avoid Qt to double delete the nodes.
     **/
    void invalidateAllNodesParenting();

    bool areKnobLinksVisible() const;
    
    void refreshNodesKnobsAtTime(SequenceTime time);
    
    void pushUndoCommand(QUndoCommand* command);
    
    bool areOptionalInputsAutoHidden() const;
    
    void copyNodesAndCreateInGroup(const std::list<boost::shared_ptr<NodeGui> >& nodes,
                                   const boost::shared_ptr<NodeCollection>& group);

    virtual void onNodesCleared() OVERRIDE FINAL;
    
    void setLastSelectedViewer(ViewerTab* tab);
    
    ViewerTab* getLastSelectedViewer() const;
    
    /**
     * @brief Given the node, it tries to move it to the ideal position
     * according to the position of the selected node and its inputs/outputs.
     * This is used when creating a node to position it correctly.
     * It will move the inputs / outputs slightly to fit this node into the nodegraph
     * so they do not overlap.
     **/
    void moveNodesForIdealPosition(boost::shared_ptr<NodeGui> n,bool autoConnect);
    
public Q_SLOTS:

    void deleteSelection();

    void connectCurrentViewerToSelection(int inputNB);

    void updateCacheSizeText();

    void showMenu(const QPoint & pos);

    void toggleCacheInfo();

    void togglePreviewsForSelectedNodes();

    void toggleAutoPreview();
    
    void toggleSelectedNodesEnabled();

    void forceRefreshAllPreviews();

    void toggleKnobLinksVisible();

    void switchInputs1and2ForSelectedNodes();
    
    void extractSelectedNode();
    
    void createGroupFromSelection();

    ///All these actions also work for backdrops
    /////////////////////////////////////////////
    ///Copy selected nodes to the clipboard, wiping previous clipboard
    void copySelectedNodes();

    void cutSelectedNodes();
    void pasteNodeClipBoards();
    void duplicateSelectedNodes();
    void cloneSelectedNodes();
    void decloneSelectedNodes();
    /////////////////////////////////////////////

    void centerOnAllNodes();

    void toggleConnectionHints();
    
    void toggleAutoHideInputs(bool setSettings = true);
    
    ///Called whenever the time changes on the timeline
    void onTimeChanged(SequenceTime time,int reason);
    
    void onGuiFrozenChanged(bool frozen);

    void onNodeCreationDialogFinished();

    void popFindDialog(const QPoint& pos = QPoint(0,0));
    
    void popRenameDialog(const QPoint& pos = QPoint(0,0));
    
    void onFindNodeDialogFinished();
    
    void refreshAllKnobsGui();
        
    void onNodeNameEditDialogFinished();
    
    void toggleAutoTurbo();
    
    void onGroupNameChanged(const QString& name);
    void onGroupScriptNameChanged(const QString& name);
    
    
    
private:
    
    bool isNearbyNavigator(const QPoint& widgetPos,QPointF& scenePos) const;

    void setVisibleNodeDetails(bool visible);
    
    virtual void enterEvent(QEvent* e) OVERRIDE FINAL;
    virtual void leaveEvent(QEvent* e) OVERRIDE FINAL;
    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE FINAL;
    virtual void keyReleaseEvent(QKeyEvent* e) OVERRIDE FINAL;
    virtual bool event(QEvent* e) OVERRIDE FINAL;
    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseDoubleClickEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void resizeEvent(QResizeEvent* e) OVERRIDE FINAL;
    virtual void paintEvent(QPaintEvent* e) OVERRIDE FINAL;
    virtual void wheelEvent(QWheelEvent* e) OVERRIDE FINAL;
    virtual void dropEvent(QDropEvent* e) OVERRIDE FINAL;
    virtual void dragEnterEvent(QDragEnterEvent* e) OVERRIDE FINAL;
    virtual void dragMoveEvent(QDragMoveEvent* e) OVERRIDE FINAL;
    virtual void dragLeaveEvent(QDragLeaveEvent* e) OVERRIDE FINAL;
    virtual void focusInEvent(QFocusEvent* e) OVERRIDE FINAL;
    virtual void focusOutEvent(QFocusEvent* e) OVERRIDE FINAL;

private:
    
    void wheelEventInternal(bool ctrlDown,double delta);

    boost::scoped_ptr<NodeGraphPrivate> _imp;
};


struct FindNodeDialogPrivate;
class FindNodeDialog : public QDialog
{
    Q_OBJECT
    
public:
    
    FindNodeDialog(NodeGraph* graph,QWidget* parent);
    
    virtual ~FindNodeDialog();
    
public Q_SLOTS:
    
    void onOkClicked();
    void onCancelClicked();
    
    void updateFindResults(const QString& filter);
    
    void updateFindResultsWithCurrentFilter();
    void forceUpdateFindResults();
private:
    
    
    void selectNextResult();
    
    virtual void changeEvent(QEvent* e) OVERRIDE FINAL;
    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE FINAL;
    
    boost::scoped_ptr<FindNodeDialogPrivate> _imp;
};

struct EditNodeNameDialogPrivate;
class EditNodeNameDialog: public QDialog
{
    Q_OBJECT
    
public:
    
    EditNodeNameDialog(NodeGraph* graph,const boost::shared_ptr<NodeGui>& node,QWidget* parent);
    
    virtual ~EditNodeNameDialog();
    
private:
    
    virtual void changeEvent(QEvent* e) OVERRIDE FINAL;
    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE FINAL;
    
    boost::scoped_ptr<EditNodeNameDialogPrivate> _imp;
};

#endif // NATRON_GUI_NODEGRAPH_H_
