#include "MainWindow.h"
#include "LastFilterJob.h"
#include "LayerPanel.h"
#include "PreviewComboBox.h"
#include "PropertyControls.h"
#include "ProjectChrome.h"
#include "EditorIcons.h"
#include "VisualStyle.h"
#include "PaletteSwatches.h"
#include <QSettings>
#include <QToolButton>
#include <QStyle>
#include <QLineEdit>
#include <QLabel>
#include <QHBoxLayout>
#include <QShortcut>
#include <QSizePolicy>
#include <QApplication>
#include <QMenuBar>
#include <QStatusBar>
#include <QDockWidget>
#include <QToolBar>
#include <QFormLayout>
#include <QGridLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QPlainTextEdit>
#include <cmath>
#include <QColorDialog>
#include <QClipboard>
#include <QImageReader>
#include <QImageWriter>
#include <QSaveFile>
#include <QCloseEvent>
#include <QSignalBlocker>
#include <QScreen>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace compositor {
namespace {
class ResponsiveToolRail final : public QScrollArea {
public:
    explicit ResponsiveToolRail(QToolBar* bar) : QScrollArea(bar), bar_(bar), contents_(new QWidget), grid_(new QGridLayout(contents_)) {
        setObjectName("responsiveToolRail");
        viewport()->setObjectName("toolRailViewport");
        contents_->setObjectName("toolRailContents");
        setStyleSheet("QScrollArea#responsiveToolRail, QWidget#toolRailViewport, QWidget#toolRailContents { background: transparent; border: 0; }");
        setFrameShape(QFrame::NoFrame);
        setWidgetResizable(false);
        setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumSize(0, 0);
        grid_->setContentsMargins(0, 0, 0, 0);
        grid_->setHorizontalSpacing(2);
        grid_->setVerticalSpacing(2);
        grid_->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
        setWidget(contents_);
    }
    void addTool(QToolButton* button) { buttons_.push_back(button); }
    void setPalette(QWidget* palette) { palette_ = palette; updateLayout(); }
protected:
    void resizeEvent(QResizeEvent* event) override { QScrollArea::resizeEvent(event); updateLayout(); }
    bool event(QEvent* event) override {
        const bool result = QScrollArea::event(event);
        if (event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange ||
            event->type() == QEvent::ScreenChangeInternal || event->type() == QEvent::Show)
            QTimer::singleShot(0, this, [this] { updateLayout(); });
        return result;
    }
private:
    void updateLayout() {
        if (!palette_ || buttons_.empty() || viewport()->height() <= 0) return;
        const int spacing = grid_->verticalSpacing();
        const auto effectiveSize = [](QWidget* widget) {
            return widget->sizeHint().expandedTo(widget->minimumSize()).boundedTo(widget->maximumSize());
        };
        int oneColumnHeight = effectiveSize(palette_).height() + int(buttons_.size()) * spacing;
        for (auto* button : buttons_) oneColumnHeight += effectiveSize(button).height();
        auto apply=[&](int columns){
            if (columns != columns_) {
                columns_ = columns;
                for (size_t index = 0; index < buttons_.size(); ++index)
                    grid_->addWidget(buttons_[index], int(index) / columns, int(index) % columns, Qt::AlignHCenter);
                grid_->addWidget(palette_, (int(buttons_.size()) + columns - 1) / columns, 0, 1, columns, Qt::AlignHCenter);
            }
            grid_->invalidate();
            const QSize required = grid_->minimumSize();
            const int contentWidth = qMax(columns == 1 ? 40 : 74, required.width());
            contents_->setFixedSize(contentWidth, required.height());
            // Include the styled toolbar layout margins, not just its CSS padding.
            const auto margins = bar_->layout()->contentsMargins();
            const int scrollbarWidth = required.height() > viewport()->height()
                ? verticalScrollBar()->sizeHint().width() : 0;
            const int railWidth = contentWidth + margins.left() + margins.right()
                + 2 * frameWidth() + scrollbarWidth;
            if (bar_->width() != railWidth) bar_->setFixedWidth(railWidth);
        };
        apply(viewport()->height() > oneColumnHeight ? 1 : 2);
        if (columns_ == 1 && contents_->height() > viewport()->height()) apply(2);
    }
    QToolBar* bar_;
    QWidget* contents_;
    QGridLayout* grid_;
    std::vector<QToolButton*> buttons_;
    QWidget* palette_{};
    int columns_{};
};
}
static QImage imageOf(const Raster&r){auto bytes=r.rgba();return QImage(bytes.data(),r.width,r.height,r.width*4,QImage::Format_RGBA8888_Premultiplied).copy();}
EditorProject* MainWindow::current(){int i=tabs_->currentIndex();return i<0||i>=int(projects_.size())?nullptr:projects_[i].get();}
Layer* MainWindow::active(){auto*p=current();if(!p||!p->document)return nullptr;for(auto&l:p->document->layers)if(l.id==p->active)return &l;return nullptr;}
bool MainWindow::canEditLayers(){const auto state=commandState();return !state.modalDialog&&ui::canEditLayers(state)&&shapeDraftId_.empty();}
bool MainWindow::canEditAppearance(){return canEditLayers()&&active()&&!active()->group&&current()->selected.size()==1;}
QAction* MainWindow::action(QMenu* menu,const QString& name,const QKeySequence& key,std::function<void()> invoke){
    auto* item=menu->addAction(name);if(!key.isEmpty())item->setShortcut(key);
    bindCommand(item,menu->title(),name,std::move(invoke));return item;
}
MainWindow::MainWindow(bool warp):warp_(warp){movePreviewJob_=new ui::MovePreviewJob([this](ui::MovePreviewJob::Result result){applyMovePreview(std::move(result));},this);viewportComposite_=new ui::ViewportCompositeJob([this](ui::ViewportCompositeJob::Result result){applyViewportComposite(std::move(result));},this);ui::installVisualStyle();setWindowTitle("Compositor");resize(1280,820);setAcceptDrops(true);tabs_=new QTabWidget;tabs_->setTabsClosable(true);tabs_->setDocumentMode(true);setCentralWidget(tabs_);tabs_->setMovable(true);connect(tabs_,&QTabWidget::currentChanged,this,[this]{switchProject();});connect(tabs_,&QTabWidget::tabCloseRequested,this,[this](int i){closeProject(i);});
initializeCommands();
auto*file=menuBar()->addMenu("&文件");action(file,"&新画布…",QKeySequence::New,[this]{newDialog();});action(file,"&导入文件…",{},[this]{importImage();});action(file,"&焦点叠加图像…",{},[this]{focusStackImages();});action(file,"&导出文件…",QKeySequence("Ctrl+Shift+E"),[this]{exportImage();});file->addSeparator();action(file,"关闭方案",QKeySequence::Close,[this]{closeProject(tabs_->currentIndex());});action(file,"退出",QKeySequence("Alt+F4"),[this]{close();});
action(file,"&打开方案…",QKeySequence::Open,[this]{openProjectDialog();});setupRecentProjectsMenu(file);action(file,"&保存方案",QKeySequence::Save,[this]{saveProject();});action(file,"项目另存为(&A)…",QKeySequence::SaveAs,[this]{saveProject(true);});
auto*editMenu=menuBar()->addMenu("&编辑");undo_=action(editMenu,"撤销",QKeySequence::Undo,[this]{if(textSession_){undoTextTyping(false);return;}if(drawingOriginal_){cancelGradient();return;}if(auto*p=current())if(auto s=p->history.undo()){restoreHistorySnapshot(std::move(*s));}});redo_=action(editMenu,"重做",QKeySequence::Redo,[this]{if(textSession_){undoTextTyping(true);return;}if(auto*p=current())if(auto s=p->history.redo()){restoreHistorySnapshot(std::move(*s));}});
auto*layerMenu=menuBar()->addMenu("&图层");action(layerMenu,"新建图层",QKeySequence("Ctrl+Shift+N"),[this]{newBlankLayer();});action(layerMenu,"复制图层",QKeySequence("Ctrl+Shift+J"),[this]{layerCommand(3);});action(layerMenu,"重命名图层…",{},[this]{auto*l=active();if(!l)return;bool ok;auto name=QInputDialog::getText(this,"重命名图层","名字",QLineEdit::Normal,QString::fromStdString(l->name),&ok).trimmed();if(ok&&!name.isEmpty())edit("Rename Layer",[&](Document&){active()->name=name.toStdString();});});action(layerMenu,"删除图层",{},[this]{layerCommand(6);});action(layerMenu,"上移图层",QKeySequence("Ctrl+]"),[this]{layerCommand(4);});action(layerMenu,"下移图层",QKeySequence("Ctrl+["),[this]{layerCommand(5);});
auto*imageMenu=menuBar()->addMenu("&图像");action(imageMenu,"填充…",{},[this]{pixelEdit(0);});action(imageMenu,"反相",QKeySequence("Ctrl+I"),[this]{pixelEdit(2);});action(imageMenu,"修剪",{},[this]{edit("Trim",[](Document&d){d=editing::trimDocument(d);});});action(imageMenu,"顺时针旋转90°",{},[this]{edit("Rotate 90° Clockwise",[](Document&d){d=editing::rotateCanvas(d,true);});if(canvas())canvas()->fit();});action(imageMenu,"逆时针旋转90°",{},[this]{edit("Rotate 90° Counterclockwise",[](Document&d){d=editing::rotateCanvas(d,false);});if(canvas())canvas()->fit();});
auto*view=menuBar()->addMenu("&预览");action(view,"适配画布",QKeySequence("Ctrl+0"),[this]{if(canvas())canvas()->fit();});action(view,"实际像素",QKeySequence("Ctrl+1"),[this]{if(canvas())canvas()->zoomAt(1,canvas()->contentRect().center());});auto*grid=action(view,"像素网格",{},[this]{if(canvas()){canvas()->showPixelGrid=!canvas()->showPixelGrid;canvas()->update();}});grid->setCheckable(true);auto*rulersAction=action(view,"标尺",QKeySequence("Ctrl+R"),[this]{if(auto* item=findChild<QAction*>("查看标尺"))if(canvas())canvas()->setRulersVisible(item->isChecked());});rulersAction->setObjectName("viewRulers");rulersAction->setCheckable(true);rulersAction->setChecked(false);auto*guidesAction=action(view,"参考线",{},[this]{auto*p=current();if(!p||!p->document||!canvas())return;auto* guidesItem=findChild<QAction*>("viewGuides");const bool show=guidesItem&&guidesItem->isChecked();canvas()->showGuides=show;if(show&&p->document->guides.empty())edit("Add Guide",[p](Document&d){d.guides.push_back({newId(),true,d.height/2.});});canvas()->guides.clear();if(canvas()->showGuides&&p->document)for(const auto&guide:p->document->guides)canvas()->guides.push_back({guide.horizontal,guide.position});canvas()->update();});guidesAction->setObjectName("查看指南");guidesAction->setCheckable(true);setupLayoutGridActions(view);setupCmykActions(view);auto*canvasOnly=action(view,"仅画布全屏",QKeySequence("F11"),[this]{setCanvasOnly(!canvasOnly_.has_value());});canvasOnly->setObjectName("viewCanvasOnly");canvasOnly->setCheckable(true);addAction(canvasOnly);auto*help=menuBar()->addMenu("&帮助");action(help,"Command Palette…",QKeySequence("Ctrl+Shift+P"),[this]{openCommandPalette();});action(help,"快捷键…",{},[this]{openShortcutEditor();});action(help,"新内容",{},[this]{showWhatsNew();});action(help,"检查更新…",{},[this]{checkForUpdates();});action(help,"关于 Compositor",{},[this]{QMessageBox::about(this,"Compositor",QString("<h3>Compositor Windows %1</h3><p>Robbie Tilton 的 Compositor 的独立 Windows 移植版.</p><p>使用图层、蒙版、选区、绘画、调整、滤镜和背景去除来创作图像。导入和处理 RAW 图像，结合多张照片进行焦点叠加，并保存可编辑的 .comp 项目。</p><p>维护者 <a href=\"https://x.com/Cengokill\">@Cengokill</a>, Compositor Windows 会定期更新，遵循 Compositor 的更新历史，大约每周发布一次。完整的 Mac 等效性和项目交换尚未验证。</p><p><a href=\"https://github.com/robbietilton/Compositor\">Original Compositor 项目</a><br>版权所有 Wonder Assembly LLC。MIT 源代码。</p><p>这个独立的移植版本尚未得到上游作者的认可。</p><p>此包附带了 Qt 和其他依赖的说明。</p>").arg(QCoreApplication::applicationVersion().toHtmlEscaped()));});
auto* tools = addToolBar("工具");
tools->setObjectName("tools");
tools->setMovable(false);
tools->setFloatable(false);
tools->setFixedWidth(56);
tools->setIconSize({22,22});
tools->setToolButtonStyle(Qt::ToolButtonIconOnly);
tools->setStyleSheet(R"(
    QToolBar#tools { border: 0; border-right: 1px solid palette(mid); padding: 8px 7px; spacing: 2px; }
    QToolBar#tools QToolButton[editorToolButton="true"] {
        background: transparent; border: 1px solid transparent; border-radius: 7px; padding: 0;
        min-width: 36px; max-width: 36px; min-height: 36px; max-height: 36px;
    }
    QToolBar#tools QToolButton[editorToolButton="true"]:hover { background: rgba(128,128,128,32); }
    QToolBar#tools QToolButton[editorToolButton="true"]:checked {
        background: rgba(128,128,128,55); border: 1px solid rgba(128,128,128,80);
    }
    QToolBar#tools QToolButton[editorToolButton="true"]:focus { border: 1px solid palette(highlight); }
)");
addToolBar(Qt::LeftToolBarArea,tools);
auto* rail = new ResponsiveToolRail(tools);
tools->addWidget(rail);
struct ToolEntry { const char* label; Tool tool; ui::EditorIcon icon; };
const ToolEntry entries[]{
    {"移动 (V)",Tool::Move,ui::EditorIcon::Move},
    {"选框 (M)",Tool::Marquee,ui::EditorIcon::Marquee},
    {"套索 (L)",Tool::Lasso,ui::EditorIcon::Lasso},
    {"多边形",Tool::Polygon,ui::EditorIcon::Polygon},
    {"魔棒 (W)",Tool::Wand,ui::EditorIcon::Wand},
    {"裁剪 (C)",Tool::Crop,ui::EditorIcon::Crop},
    {"画笔 (B)",Tool::Brush,ui::EditorIcon::Brush},
    {"擦除 (E)",Tool::Eraser,ui::EditorIcon::Eraser},
    {"修复 (J)",Tool::SpotHealing,ui::EditorIcon::Heal},
    {"仿制 (S)",Tool::CloneStamp,ui::EditorIcon::Clone},
    {"模糊 (R)",Tool::Blur,ui::EditorIcon::Retouch},
    {"渐变 (G)",Tool::Gradient,ui::EditorIcon::Gradient},
    {"形状 (U)",Tool::Shape,ui::EditorIcon::Shape},
    {"文字 (T)",Tool::Text,ui::EditorIcon::Type},
    {"吸管 (I)",Tool::Eyedropper,ui::EditorIcon::Eyedropper},
    {"抓取 (H)",Tool::Hand,ui::EditorIcon::Hand},
    {"缩放 (Z)",Tool::Zoom,ui::EditorIcon::Zoom}
};
for (const auto& entry : entries) {
    const bool alternate = entry.tool==Tool::Polygon || entry.tool==Tool::Eraser;
    auto* a = new QAction(ui::editorIcon(entry.icon),entry.label,tools);
    addAction(a);
    a->setCheckable(true);
    a->setToolTip(entry.label);
    a->setProperty("editorTool",int(entry.tool));
    bindCommand(a,"Tools",entry.label,[this,t=entry.tool]{selectTool(t==Tool::Brush&&brushMode_==ProjectBrushMode::Erase?Tool::Eraser:t);});
    if(entry.tool==Tool::Brush)commands_->releaseShortcut(a);
    if(!alternate) {
        auto* button = new QToolButton;
        button->setDefaultAction(a);
        button->setIconSize({22,22});
        button->setProperty("editorToolButton",true);
        button->style()->unpolish(button);
        button->style()->polish(button);
        button->setAccessibleName(entry.label);
        button->setFixedSize(36,36);
        rail->addTool(button);
    }
}
auto*brushKey=new QAction("Brush (B)",this);bindCommand(brushKey,"Tools","Brush (B)",[this]{selectTool(Tool::Brush);});addAction(brushKey);
setupPaletteControls(rail);
rail->setPalette(paletteSwatches_);
setupGifTimeline();
auto*dock=new QDockWidget("图层",this);dock->setObjectName("layersDock");auto* layerHeader=new QWidget(dock);layerHeader->setObjectName("sectionTitle");auto* layerHeaderLayout=new QHBoxLayout(layerHeader);layerHeaderLayout->setContentsMargins(8,2,8,2);layerHeaderLayout->setSpacing(4);
auto* layerTitle=new QLabel("图层",layerHeader);layerTitle->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);auto* layerSearch=new QLineEdit(layerHeader);layerSearch->setMinimumWidth(0);layerSearch->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);layerSearch->setObjectName("layerSearch");layerSearch->setAccessibleName("按名称搜索图层");layerSearch->setPlaceholderText("搜索图层");layerSearch->hide();
auto* layerSearchButton=new QToolButton(layerHeader);layerSearchButton->setObjectName("layerSearchButton");layerSearchButton->setText("搜索");layerSearchButton->setFixedWidth(54);layerSearchButton->setStyleSheet("QToolButton { padding-left: 3px; padding-right: 3px; }");layerSearchButton->setAccessibleName("搜索图层");layerSearchButton->setToolTip("按名称搜索图层");
auto* layerSelected=new QToolButton(layerHeader);layerSelected->setObjectName("layerSelectedFilter");layerSelected->setText("仅选中");layerSelected->setFixedWidth(70);layerSelected->setStyleSheet("QToolButton { padding-left: 3px; padding-right: 3px; }");layerSelected->setCheckable(true);layerSelected->setAccessibleName("仅显示选中图层");layerSelected->setToolTip("仅显示选中图层");
layerHeaderLayout->addWidget(layerTitle,1);layerHeaderLayout->addWidget(layerSearch,1);layerHeaderLayout->addWidget(layerSearchButton);layerHeaderLayout->addWidget(layerSelected);dock->setTitleBarWidget(layerHeader);
auto refreshLayerFilter=[this,layerSearch,layerSelected]{if(auto* controller=ui::LayerPanelController::find(layers_))controller->setFilter(layerSearch->text(),layerSelected->isChecked());};
connect(layerSearch,&QLineEdit::textChanged,this,[refreshLayerFilter](const QString&){refreshLayerFilter();});connect(layerSelected,&QToolButton::toggled,this,[refreshLayerFilter](bool){refreshLayerFilter();});
auto closeLayerSearch=[layerSearch,layerTitle,layerSearchButton]{layerSearch->clear();layerSearch->hide();layerTitle->show();layerSearchButton->setText("搜索");layerSearchButton->setAccessibleName("搜索图层");layerSearchButton->setToolTip("按名称搜索图层");};
connect(layerSearchButton,&QToolButton::clicked,this,[layerSearch,layerTitle,layerSearchButton,closeLayerSearch]{if(!layerSearch->isHidden()){closeLayerSearch();return;}layerTitle->hide();layerSearch->show();layerSearchButton->setText("关闭");layerSearchButton->setAccessibleName("关闭图层搜索");layerSearchButton->setToolTip("关闭图层搜索");layerSearch->setFocus();});
auto* layerSearchEscape=new QShortcut(QKeySequence(Qt::Key_Escape),layerSearch);layerSearchEscape->setContext(Qt::WidgetShortcut);connect(layerSearchEscape,&QShortcut::activated,this,[layerSearch,closeLayerSearch]{if(layerSearch->text().isEmpty())closeLayerSearch();else layerSearch->clear();});
auto*panel=new QWidget;auto*layout=new QVBoxLayout(panel);auto*blendPicker=new ui::PreviewComboBox;blend_=blendPicker;blendPicker->preview=[this](int index){if(index>=0&&index<int(blendNames.size()))previewBlendMode(Blend(index));};blendPicker->dismissed=[this]{previewBlendMode({});};blend_->setObjectName("layerBlend");blend_->setAccessibleName("图层混合模式");for(auto n:blendNames)blend_->addItem(n);layout->addWidget(blend_);opacity_=new ui::TrackSlider(Qt::Horizontal);opacity_->setObjectName("layerOpacity");opacity_->setRange(0,100);opacity_->setDefaultValue(100);opacity_->withdrawPressStep=[this]{withdrawOpacityPress();};opacity_->setAccessibleName("图层不透明度");layout->addWidget(createLayerOpacityControl());layers_=new QTreeWidget;layers_->setHeaderHidden(true);layers_->setAccessibleName("图层列表");layout->addWidget(layers_);dock->setWidget(panel);addDockWidget(Qt::RightDockWidgetArea,dock);connect(layers_,&QTreeWidget::itemChanged,this,[this](QTreeWidgetItem*i){if(refreshing_||!i)return;auto id=i->data(0,Qt::UserRole).toString().toStdString();bool visible=i->checkState(0)==Qt::Checked;edit("Layer Visibility",[&](Document&d){for(auto&l:d.layers)if(l.id==id){l.visible=visible;if(!i->text(0).trimmed().isEmpty())l.name=i->text(0).trimmed().toStdString();}});});connect(blend_,&QComboBox::currentIndexChanged,this,[this](int i){if(!refreshing_&&i>=0&&i<int(blendNames.size()))setLayerBlendMode(Blend(i));});connect(opacity_,&QSlider::sliderPressed,this,[this]{opacityPressUndoCount_.reset();if(!canEditAppearance())return;finishOpacityEdit();opacityOwner_=current();if(opacityOwner_){opacityPressUndoCount_=opacityOwner_->history.undoCount();opacityOwner_->history.begin("Layer Opacity",opacityOwner_->document,opacityOwner_->active);}});connect(opacity_,&QSlider::sliderReleased,this,[this]{finishOpacityEdit();refresh(false);});connect(opacity_,&QSlider::valueChanged,this,[this](int v){if(!refreshing_&&canEditAppearance())edit("Layer Opacity",[&](Document&){active()->opacity=v/100.;});});
auto*inspector=new QDockWidget("变换",this);inspector->setObjectName("transformDock");auto*formWidget=new QWidget;auto*form=new QFormLayout(formWidget);const char*names[]{"X","Y","宽度","高度","角度"};for(int i=0;i<5;++i){auto*spin=new ui::PropertyNumber;spin->releaseFocus=[this]{if(canvas())canvas()->setFocus();};spin->setCompactDisplay(true);spin->setDecimals(3);spin->setRange(i==2||i==3?1:-1000000,i==2||i==3?300000:1000000);spin->setAccessibleName(names[i]);geometry_[i]=spin;form->addRow(names[i],spin);connect(spin,&QDoubleSpinBox::valueChanged,this,[this,i](double value){if(!refreshing_)editTransformGeometry(i,value);});}inspector->setWidget(formWidget);addDockWidget(Qt::RightDockWidgetArea,inspector);setupBrushControls();setupAdjustmentActions();setupSelectionActions();setupLayerActions();setupTransformActions();setupDrawingActions();setupTypeControls();setupRetouchActions();{QSettings settings;commands_->restoreShortcuts(settings);}statusBar()->showMessage("创建画布或导入图像");ui::installProjectChrome(this,tabs_,dock,layers_);ui::styleWorkspace(this);refresh(false);addEmptyProject(false);}
void MainWindow::restoreHistorySnapshot(Snapshot snapshot){
    stopGifPlayback();
    beginPreviewGesture();
    invalidateViewportComposite();
    auto* project=current();if(!project)return;
    if(project->canvas)project->canvas->setCompositeHeld(false);
    const bool keepDraft=selectionGesture_.active()&&selectionOwner_==project;
    auto draft=keepDraft?std::optional(std::move(selectionGesture_)):std::nullopt;
    auto* pointer=pointerOwner_;const auto scrollPoint=selectionScrollPoint_;
    const bool scrolling=selectionScrollTimer_&&selectionScrollTimer_->isActive();
    pointerCancel();project->document=std::move(snapshot.document);project->active=std::move(snapshot.activeLayer);
    if(draft){selectionGesture_=std::move(*draft);selectionOwner_=project;if(pointer==project)pointerOwner_=project;
        selectionScrollPoint_=scrollPoint;if(scrolling&&project->document)selectionScrollTimer_->start();}
    refresh();
}
void MainWindow::finishOpacityEdit(){auto*owner=opacityOwner_;opacityOwner_=nullptr;if(!owner)return;try{owner->history.end(owner->document,owner->active);}catch(...){if(auto snapshot=owner->history.cancel()){owner->document=std::move(snapshot->document);owner->active=std::move(snapshot->activeLayer);}throw;}}
// A double-click reaches the track after the first click's press already committed a snap.
// Undo that step through the Undo path so the reset becomes the only history entry.
void MainWindow::withdrawOpacityPress(){
    const auto pressCount=opacityPressUndoCount_;opacityPressUndoCount_.reset();
    auto* project=current();
    if(!project||!pressCount||project->history.undoCount()!=*pressCount+1)return;
    if(auto snapshot=project->history.undo())restoreHistorySnapshot(std::move(*snapshot));
}
void MainWindow::selectTool(Tool value){
    if(!ui::commandEnabled(ui::CommandGate::Tool,commandState()))return;
    if(textSession_&&value!=Tool::Text&&!finishText())return;
    if(value==Tool::Brush||value==Tool::Eraser)brushMode_=value==Tool::Eraser?ProjectBrushMode::Erase:ProjectBrushMode::Paint;
    if(value==Tool::Lasso&&lassoKind_==editing::LassoKind::Polygonal)value=Tool::Polygon;
    if(value==Tool::Polygon)lassoKind_=editing::LassoKind::Polygonal;
    if(value!=tool_){applyGradient();if(transformSession_&&transformSession_->persistent)applyTransformSession();pointerCancel();const bool leftHand=tool_==Tool::Hand;tool_=value;if(canvas()&&tool_==Tool::Hand)canvas()->setCursor(Qt::OpenHandCursor);else if(canvas()&&leftHand)canvas()->unsetCursor();if(canvas())canvas()->updateHoverCursorAtPointer();}
    if(value==Tool::Crop&&!cropDraft_)if(auto* p=current();p&&p->document){cropRatioChoice_="Free";cropDraft_=editing::Rect{0,0,double(p->document->width),double(p->document->height)};if(p->document->selection&&p->document->selection->coverage){auto bounds=editing::coverageBounds(*p->document->selection->coverage);if(!bounds.empty())cropDraft_=bounds;}}
    if(auto* box=findChild<QComboBox*>("lassoKind")){const QSignalBlocker block(box);box->setCurrentIndex(lassoKind_==editing::LassoKind::Polygonal?1:0);}
    if(auto* box=findChild<QComboBox*>("cropRatioChoice")){const QSignalBlocker block(box);box->setCurrentText(cropRatioChoice_);}
    refresh(false,false);
}
void MainWindow::edit(const char*name,const std::function<void(Document&)>&fn,bool rebuildLayers){stopGifPlayback();auto*p=current();if(!p||!p->document||p->importing||p->projectBusy)return;p->history.begin(name,p->document,p->active);try{fn(*p->document);validateDocument(*p->document);p->history.end(p->document,p->active);}catch(...){if(auto s=p->history.cancel()){p->document=std::move(s->document);p->active=std::move(s->activeLayer);}throw;}refresh(true,rebuildLayers);}
void MainWindow::setCanvasOnly(bool enabled){
    // A held canvas gesture owns mouse capture and focus, so hiding chrome now would interrupt it.
    if(enabled!=canvasOnly_.has_value()&&!(canvas()&&canvas()->pointerHeld())){
        if(enabled){
            CanvasOnlyState state{isMaximized(),normalGeometry(),QPointer<QWidget>(QApplication::focusWidget()),{}};
            std::vector<QWidget*> chrome{menuBar(),statusBar(),tabs_->tabBar()};
            for(auto* bar:findChildren<QToolBar*>(QString(),Qt::FindDirectChildrenOnly))chrome.push_back(bar);
            for(auto* dock:findChildren<QDockWidget*>(QString(),Qt::FindDirectChildrenOnly))chrome.push_back(dock);
            // Only chrome that is visible now is hidden and later shown, so panels the user already closed stay closed.
            for(auto* widget:chrome)if(!widget->isHidden()){state.restoreVisible.push_back(widget);widget->hide();}
            // Qt does not trigger a menu action's shortcut while its menu bar is hidden, so the window takes those shortcuts until exit.
            for(auto* action:menuBar()->findChildren<QAction*>())if(!action->shortcuts().isEmpty()&&!actions().contains(action)){addAction(action);state.borrowedActions.push_back(action);}
            canvasOnly_=std::move(state);
            showFullScreen();
            if(canvas())canvas()->setFocus();
        }else{
            auto state=std::move(*canvasOnly_);
            canvasOnly_.reset();
            for(auto& action:state.borrowedActions)if(action)removeAction(action);
            for(auto& widget:state.restoreVisible)if(widget)widget->show();
            if(state.maximized)showMaximized();else{showNormal();setGeometry(state.normalGeometry);}
            if(state.focus&&state.focus->isVisible())state.focus->setFocus(Qt::OtherFocusReason);else if(canvas())canvas()->setFocus();
        }
        refresh(false,false);
    }
    if(auto* item=findChild<QAction*>("viewCanvasOnly"))item->setChecked(canvasOnly_.has_value());
}
void MainWindow::refreshHistoryChrome(EditorProject* project){
    const bool editable=project&&!project->importing&&!project->projectBusy;
    undo_->setEnabled(editable&&project->history.canUndo());
    redo_->setEnabled(editable&&project->history.canRedo());
    undo_->setText(project&&project->history.canUndo()?"Undo "+QString::fromStdString(project->history.undoName()):"Undo");
    redo_->setText(project&&project->history.canRedo()?"Redo "+QString::fromStdString(project->history.redoName()):"Redo");
}
void MainWindow::refresh(bool render,bool rebuildLayers){if(refreshing_)return;refreshing_=true;auto*p=current();if(p&&p->canvas)p->canvas->invalidateComposite();auto*l=active();const auto layerState=commandState(p);const bool layerEditing=canEditLayers();layers_->setEnabled(p&&p->document&&!layerState.modalDialog&&!layerState.projectBusy&&!layerState.importing);layers_->setSelectionMode(layerEditing?QAbstractItemView::ExtendedSelection:QAbstractItemView::NoSelection);layers_->setEditTriggers(layerEditing?QAbstractItemView::EditKeyPressed:QAbstractItemView::NoEditTriggers);if(!rebuildLayers)if(auto* controller=ui::LayerPanelController::find(layers_))controller->updateSelection();if(p&&p->page){p->page->setCurrentWidget(p->document?static_cast<QWidget*>(p->canvas):p->welcome);p->welcome->setEnabled(!p->importing&&!p->projectBusy);}if(p&&!p->document){p->canvas->setDocumentSize(0,0);p->selected.clear();tabs_->setTabText(tabs_->currentIndex(),p->defaultTitle+(p->history.modified()?" *":""));}if((!p||!p->document)&&rebuildLayers)refreshLayerPanel();if(p&&p->document){if(p->selected.empty()||std::find(p->selected.begin(),p->selected.end(),p->active)==p->selected.end())p->selected={p->active};if(rebuildLayers)refreshLayerPanel();
p->canvas->setBlackSurround(canvasOnly_.has_value());p->canvas->setTransformOverlay(tool_==Tool::Move&&(transformControls_||transformSession_)?selectedTransform():std::nullopt);p->canvas->setGradientLine(drawingOriginal_&&gradientOwner_==p?std::optional(std::pair{gradientStart_,gradientEnd_}):std::nullopt);p->canvas->setDistortionOverlay(tool_==Tool::Move&&transformSession_?transformSession_->corners:std::nullopt);if(auto* rulers=findChild<QAction*>("viewRulers"))p->canvas->setRulersVisible(rulers->isChecked());if(auto* guidesItem=findChild<QAction*>("viewGuides")){const QSignalBlocker block(guidesItem);guidesItem->setChecked(p->canvas->showGuides);}p->canvas->guides.clear();if(p->canvas->showGuides)for(const auto&guide:p->document->guides)p->canvas->guides.push_back({guide.horizontal,guide.position});p->canvas->setSelection(p->document->selection?p->document->selection->coverage:nullptr,p->document->selection?p->document->selection->outline:nullptr);if(render){try{p->canvas->setDocumentSize(p->document->width,p->document->height);}catch(const std::exception&e){statusBar()->showMessage(e.what());}const bool liveStroke=stroke_||bool(retouch_)||bool(transformDrag_);p->canvas->setNavigatorDeferred(liveStroke);if(!liveStroke)p->canvas->scheduleNavigator();}QString title=p->path.isEmpty()?p->defaultTitle:QFileInfo(p->path).fileName();tabs_->setTabText(tabs_->currentIndex(),title+(p->history.modified()?" *":""));statusBar()->showMessage(QString("%1 × %2 px  ·  %3 ppi").arg(p->document->width).arg(p->document->height).arg(p->document->resolution));}if(target_){QSignalBlocker block(target_);target_->setEnabled(l&&l->mask.has_value());if(p&&(!l||!l->mask))p->maskSelected=false;target_->setCurrentIndex(p&&p->maskSelected?1:0);}blend_->setEnabled(canEditAppearance());opacity_->setEnabled(canEditAppearance());refreshOpacityPercent();if(l){blend_->setCurrentIndex(int(l->blend));opacity_->setValue(int(std::lround(l->opacity*100)));auto t=selectedTransform().value_or(l->transform);double values[]{t.x,t.y,t.width,t.height,t.rotation};for(int i=0;i<5;++i)ui::synchronizeNumber(geometry_[i],values[i]);}for(auto*g:geometry_)g->setEnabled(ui::commandEnabled(ui::CommandGate::TransformDraft,commandState())&&l!=nullptr&&p&&!p->importing&&!p->projectBusy&&(!transformSession_||!transformSession_->corners));refreshHistoryChrome(p);for(const auto*name:{"applyGradient","cancelGradient"})if(auto*a=findChild<QAction*>(name)){a->setVisible(drawingOriginal_.has_value());a->setEnabled(drawingOriginal_.has_value());}updateTransformActions();refreshBrushControls();refreshRetouchControls();refreshBrushPointer();for(auto*bar:findChildren<QToolBar*>()){auto name=bar->objectName();if(!canvasOnly_){if(name=="wandOptions")bar->setVisible(tool_==Tool::Wand);else if(name=="brushOptions")bar->setVisible(tool_==Tool::Brush||tool_==Tool::Eraser);else if(name=="selectionOptions")bar->setVisible(tool_==Tool::Marquee||tool_==Tool::Lasso||tool_==Tool::Polygon||tool_==Tool::Wand);else if(name=="transformOptions")bar->setVisible(tool_==Tool::Move);else if(name=="drawingOptions")bar->setVisible(tool_==Tool::Gradient||tool_==Tool::Shape||tool_==Tool::Crop||(active()&&!active()->shapeJson.empty()));else if(name=="typeOptions")bar->setVisible(tool_==Tool::Text||bool(textSession_));}if(name=="tools")for(auto*a:bar->findChildren<QAction*>())if(a->property("editorTool").isValid()){
    const auto actionTool=Tool(a->property("editorTool").toInt());
    a->setChecked(actionTool==tool_||(actionTool==Tool::Brush&&tool_==Tool::Eraser)||(actionTool==Tool::Lasso&&tool_==Tool::Polygon));
    if(actionTool==Tool::Marquee)a->setIcon(ui::editorIcon(ellipse_?ui::EditorIcon::EllipseMarquee:ui::EditorIcon::Marquee));
    if(actionTool==Tool::Lasso)a->setIcon(ui::editorIcon(lassoKind_==editing::LassoKind::Polygonal?ui::EditorIcon::Polygon:ui::EditorIcon::Lasso));
    if(actionTool==Tool::Brush)a->setIcon(ui::editorIcon(brushMode_==ProjectBrushMode::Erase?ui::EditorIcon::Eraser:ui::EditorIcon::Brush));
}}
for(auto*a:findChildren<QAction*>())if(a->property("editorTool").isValid()){
    const auto actionTool=Tool(a->property("editorTool").toInt());
    a->setChecked(actionTool==tool_||(actionTool==Tool::Brush&&tool_==Tool::Eraser)||(actionTool==Tool::Lasso&&tool_==Tool::Polygon));
}
refreshCmykActions();refreshing_=false;refreshSelectionGesture();if(commands_)commands_->refresh();refreshPaletteControls();
    for(const char* name:{"selectionExpandAmount","selectionContractAmount"})if(auto* control=findChild<QSpinBox*>(name))control->setEnabled(ui::commandEnabled(ui::CommandGate::ModifySelection,commandState()));
    if(auto* control=findChild<QComboBox*>("cropRatioChoice")){control->setVisible(tool_==Tool::Crop);control->setEnabled(p&&p->document&&!p->projectBusy&&!p->importing);}
    if(auto* control=findChild<QComboBox*>("lassoKind"))control->setVisible(tool_==Tool::Lasso||tool_==Tool::Polygon);
    QStringList titles;for(const auto& project:projects_)titles.push_back(project->path.isEmpty()?project->defaultTitle:QFileInfo(project->path).fileName());ui::refreshProjectTabs(tabs_,canSwitchProjects(),titles);
    refreshShapeControls();refreshCropControls();refreshTypeControls();refreshGifTimeline();
}
bool MainWindow::closeProject(int i){
    if(i<0||i>=int(projects_.size()))return true;
    if(auto* job=ui::LastFilterJob::find(this,projects_[i].get())){
        delete job;projects_[i]->projectBusy=false;
    }
    if(!canSwitchProjects()||projects_[i]->importing||projects_[i]->projectBusy)return false;
    if(auto* panel=ui::LayerPanelController::find(layers_))panel->finishVisibilitySwipe();finishVisibilitySwipe();
    finishOpacityEdit();finishShapeStyleEdit();if(textSession_&&textSession_->owner==projects_[i].get())finishText();applyGradient();if(transformSession_&&transformSession_->persistent)applyTransformSession();pointerCancel();
    auto&p=*projects_[i];
    // A write in flight must finish before deciding. Edits made after its snapshot still prompt again.
    for(;;){
        if(!p.saveJob.isNull()&&!waitForProjectSave(p))return false;
        if(!p.history.modified())break;
        tabs_->setCurrentIndex(i);
        auto answer=QMessageBox::warning(this,"未保存的更改","关闭此方案前是否保存更改？",QMessageBox::Save|QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Save);
        if(answer==QMessageBox::Cancel)return false;
        if(answer==QMessageBox::Discard)break;
        try{if(!saveProject())return false;}catch(const std::exception&e){QMessageBox::critical(this,"保存方案",e.what());return false;}
    }
    beginPreviewGesture(p.canvas);invalidateViewportComposite();
    if(p.canvas->cancelWheelComposite)p.canvas->cancelWheelComposite();
    p.canvas->requestWheelComposite={};p.canvas->cancelWheelComposite={};
    auto*widget=p.page;p.canvas->viewportProvider={};p.canvas->pointerDown={};p.canvas->pointerDoubleClick={};p.canvas->pointerMove={};p.canvas->pointerUp={};p.canvas->pointerCancel={};p.canvas->pointerInterrupted={};p.canvas->pointerHover={};p.canvas->pointerLeave={};p.canvas->rightPointerDown={};p.canvas->rightPointerMove={};p.canvas->navigationAllowed={};p.canvas->navigatorProvider={};if(activeProject_==&p)activeProject_=nullptr;projects_.erase(projects_.begin()+i);tabs_->removeTab(i);widget->deleteLater();if(projects_.empty()&&!closingWindow_)addEmptyProject(false);else switchProject();refresh(false);return true;
}
void MainWindow::closeEvent(QCloseEvent*e){closingWindow_=true;for(int i=int(projects_.size())-1;i>=0;--i)if(!closeProject(i)){closingWindow_=false;e->ignore();return;}e->accept();}
void MainWindow::pointerBegin(QPointF p,Qt::KeyboardModifiers m,int clickCount,std::optional<double> pressure){stopGifPlayback();if(current()&&(current()->importing||current()->projectBusy))return;pointerOwner_=current();if(beginTemporaryHand({p.x(),p.y()}))return;if(beginEditPanelPointer({p.x(),p.y()},m))return;if(beginPalette({p.x(),p.y()},m))return;if(tool_==Tool::Text){beginTextPointer({p.x(),p.y()},m,clickCount);return;}if(tool_==Tool::Zoom&&canvas()){zoomDragging_=true;zoomMoved_=false;zoomStart_=canvas()->zoom;auto at=canvas()->viewMapping().toView({p.x(),p.y()});zoomAnchor_={at.x,at.y};return;}if(beginRetouch({p.x(),p.y()},m))return;if(beginDrawing({p.x(),p.y()},m))return;if(clickCount>=2&&tool_==Tool::Move&&editTextAt({p.x(),p.y()},clickCount))return;if(beginTransform({p.x(),p.y()},m))return;if(beginSelection({p.x(),p.y()},m,clickCount))return;if(beginBrush({p.x(),p.y()},m,pressure))return;if(!current())return;press_={p.x(),p.y()};panOrigin_=canvas()->pan;if(tool_==Tool::Hand)canvas()->setCursor(Qt::ClosedHandCursor);if(tool_==Tool::Move&&active()){moving_=active()->transform;current()->history.begin("Move Layer",current()->document,current()->active);}}
void MainWindow::pointerUpdate(QPointF p,Qt::KeyboardModifiers m,double pressure){if(pointerOwner_!=current()){pointerCancel();return;}if(updateTemporaryHand({p.x(),p.y()},false))return;if(tool_==Tool::Text){updateTextPointer({p.x(),p.y()},false);return;}if(updateEditPanelPointer({p.x(),p.y()},m,false))return;if(updatePalette({p.x(),p.y()},false))return;if(zoomDragging_&&canvas()){auto at=canvas()->viewMapping().toView({p.x(),p.y()});auto dx=at.x-zoomAnchor_.x();zoomMoved_|=std::abs(dx)>3;if(zoomMoved_)canvas()->zoomAt(zoomStart_*std::pow(2,dx/100),zoomAnchor_);return;}if(updateRetouch({p.x(),p.y()},m))return;if(updateDrawing({p.x(),p.y()},m,false))return;if(transformDrag_){updateTransform({p.x(),p.y()},m,false);return;}if(updateSelection({p.x(),p.y()},m,false))return;if(updateBrush({p.x(),p.y()},false,pressure))return;if(transformDrag_){updateTransform({p.x(),p.y()},m,false);return;}if(tool_==Tool::Hand){canvas()->panBy(QPointF(p.x()-press_.x,p.y()-press_.y)*canvas()->pointsPerPixel());return;}if(!moving_||!active())return;double dx=p.x()-press_.x,dy=p.y()-press_.y;if(m&Qt::ShiftModifier){if(std::abs(dx)>std::abs(dy))dy=0;else dx=0;}active()->transform.x=std::round(moving_->x+dx);active()->transform.y=std::round(moving_->y+dy);refresh();}
void MainWindow::pointerEnd(QPointF p,Qt::KeyboardModifiers m,double pressure){if(pointerOwner_!=current()){pointerCancel();return;}if(updateTemporaryHand({p.x(),p.y()},true)){pointerOwner_=nullptr;return;}if(tool_==Tool::Text){updateTextPointer({p.x(),p.y()},true);pointerOwner_=nullptr;return;}if(updateEditPanelPointer({p.x(),p.y()},m,true)){pointerOwner_=nullptr;return;}if(updatePalette({p.x(),p.y()},true)){pointerOwner_=nullptr;return;}if(zoomDragging_&&canvas()){if(!zoomMoved_)canvas()->zoomAt(zoomStart_*(m.testFlag(Qt::AltModifier)?.5:2),zoomAnchor_);zoomDragging_=false;pointerOwner_=nullptr;return;}if(endRetouch({p.x(),p.y()},m)){pointerOwner_=nullptr;return;}if(updateDrawing({p.x(),p.y()},m,true)){pointerOwner_=nullptr;return;}if(transformDrag_){updateTransform({p.x(),p.y()},m,true);pointerOwner_=nullptr;return;}if(updateSelection({p.x(),p.y()},m,true)){if(tool_!=Tool::Polygon)pointerOwner_=nullptr;return;}if(updateBrush({p.x(),p.y()},true,pressure)){pointerOwner_=nullptr;return;}if(transformDrag_){updateTransform({p.x(),p.y()},m,true);pointerOwner_=nullptr;return;}if(moving_){pointerUpdate(p,m);moving_.reset();current()->history.end(current()->document,current()->active);refresh(false);}pointerOwner_=nullptr;if(tool_==Tool::Hand&&canvas())canvas()->setCursor(Qt::OpenHandCursor);}
void MainWindow::pointerCancel(){const bool openedBrushHistory=brushHistoryOpen_;const bool activeBrush=bool(stroke_);brushHistoryOpen_=false;textSelecting_=false;textArming_=false;textHandle_=-1;textResizeChanged_=false;cancelShape();cancelBrushTip();cancelTemporaryHand();cancelGradient();samplingPalette_=false;zoomDragging_=false;if(canvas())canvas()->setSampleRing({});cancelTransformSession();cancelRetouch();drawingOriginal_.reset();shapeDraftId_.clear();cropDraft_.reset();cropDrag_.reset();cropSnap_.reset();refreshCropControls();transformDrag_.reset();transformBefore_.reset();transformIds_.clear();cancelSelectionGesture();selectionBefore_.reset();movingSelection_=false;if(stroke_){stroke_->cancel();stroke_.reset();}moving_.reset();auto*owner=pointerOwner_;pointerOwner_=nullptr;if(owner){owner->brushPreview.reset();owner->brushLayerId.clear();}if(activeBrush&&!openedBrushHistory){if(owner==current())refresh();return;}if(owner)if(auto s=owner->history.cancel()){owner->document=std::move(s->document);owner->active=std::move(s->activeLayer);if(owner==current())refresh();}if(tool_==Tool::Hand&&canvas())canvas()->setCursor(Qt::OpenHandCursor);if(canvas())canvas()->updateHoverCursorAtPointer();}
void MainWindow::addFeasibilityDocument(){Document d;d.id=newId();d.width=640;d.height=420;Layer bg;bg.id=newId();bg.name="Background";bg.transform={0,0,640,420};bg.raster=Raster::filled(640,420,{35,65,90,255});Layer l;l.id=newId();l.name="Translucent masked layer";l.transform={145.25,90.5,320,220,17};l.raster=Raster::filled(320,220,{180,75,30,200});auto mask=std::make_shared<GrayRaster>();mask->width=320;mask->height=220;mask->pixels.resize(320*220);for(int y=0;y<220;++y)for(int x=0;x<320;++x)mask->pixels[size_t(y)*320+x]=uint8_t(x*255/319);l.mask=Mask{mask};d.layers={bg,l};addProject(d,"Feasibility");}
}

