/*
    Copyright (c) 2013, Lukas Holecek <hluk@email.cz>

    This file is part of CopyQ.

    CopyQ is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    CopyQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with CopyQ.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "clipboardbrowser.h"

#include "client_server.h"
#include "clipboarddialog.h"
#include "clipboarditem.h"
#include "clipboardmodel.h"
#include "configurationmanager.h"
#include "iconfactory.h"
#include "itemdelegate.h"
#include "itemfactory.h"

#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QScrollBar>
#include <QTimer>

namespace {

const QIcon iconAction() { return getIcon("action", IconCog); }
const QIcon iconClipboard() { return getIcon("clipboard", IconPaste); }
const QIcon iconEdit() { return getIcon("accessories-text-editor", IconEdit); }
const QIcon iconEditExternal() { return getIcon("accessories-text-editor", IconPencil); }
const QIcon iconRemove() { return getIcon("list-remove", IconRemove); }
const QIcon iconShowContent() { return getIcon("dialog-information", IconInfoSign); }

bool alphaSort(const ClipboardModel::ComparisonItem &lhs,
                     const ClipboardModel::ComparisonItem &rhs)
{
    return lhs.second->text().localeAwareCompare( rhs.second->text() ) < 0;
}

bool reverseSort(const ClipboardModel::ComparisonItem &lhs,
                        const ClipboardModel::ComparisonItem &rhs)
{
    return lhs.first > rhs.first;
}

} // namespace

ClipboardBrowserShared::ClipboardBrowserShared()
    : editor()
    , maxItems(100)
    , formats(QString("text/plain"))
    , maxImageWidth(100)
    , maxImageHeight(100)
    , textWrap(false)
    , commands()
    , viMode(false)
{
}

void ClipboardBrowserShared::loadFromConfiguration()
{
    ConfigurationManager *cm = ConfigurationManager::instance();
    editor = cm->value("editor").toString();
    maxItems = cm->value("maxitems").toInt();
    formats = ItemFactory::instance()->formatsToSave();
    maxImageWidth = cm->value("max_image_width").toInt();
    maxImageHeight = cm->value("max_image_height").toInt();
    textWrap = cm->value("text_wrap").toBool();
    commands = cm->commands();
    saveOnReturnKey = !cm->value("edit_ctrl_return").toBool();
}

ClipboardBrowser::Lock::Lock(ClipboardBrowser *self) : c(self)
{
    m_autoUpdate = c->autoUpdate();
    m_updates = c->updatesEnabled();
    c->setAutoUpdate(false);
    c->setUpdatesEnabled(false);
}

ClipboardBrowser::Lock::~Lock()
{
    c->setAutoUpdate(m_autoUpdate);
    c->setUpdatesEnabled(m_updates);
}

ClipboardBrowser::ClipboardBrowser(QWidget *parent, const ClipboardBrowserSharedPtr &sharedData)
    : QListView(parent)
    , m_id()
    , m_lastFilter()
    , m_update(false)
    , m( new ClipboardModel(this) )
    , d( new ItemDelegate(viewport()) )
    , m_timerSave( new QTimer(this) )
    , m_menu( new QMenu(this) )
    , m_sharedData(sharedData ? sharedData : ClipboardBrowserSharedPtr(new ClipboardBrowserShared))
{
    setLayoutMode(QListView::Batched);
    setBatchSize(1);
    setFrameShadow(QFrame::Sunken);
    setTabKeyNavigation(false);
    setAlternatingRowColors(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setWrapping(false);
    setLayoutMode(QListView::SinglePass);
    setEditTriggers(QAbstractItemView::EditKeyPressed);
    setSpacing(5);

    m_timerSave->setSingleShot(true);
    connect( m_timerSave, SIGNAL(timeout()),
             this, SLOT(saveItems()) );

    // delegate for rendering and editing items
    setItemDelegate(d);
    connect( d, SIGNAL(rowChanged(int, const QSize &)),
             this, SLOT(onRowChanged(int, const QSize &)) );

    // set new model
    QItemSelectionModel *old_model = selectionModel();
    setModel(m);
    delete old_model;

    connect( d, SIGNAL(editingActive(bool)),
             this, SIGNAL(editingActive(bool)) );

    connect( m, SIGNAL(rowsRemoved(QModelIndex,int,int)),
             d, SLOT(rowsRemoved(QModelIndex,int,int)) );
    connect( m, SIGNAL(rowsInserted(QModelIndex, int, int)),
             d, SLOT(rowsInserted(QModelIndex, int, int)) );
    connect( m, SIGNAL(rowsMoved(QModelIndex, int, int, QModelIndex, int)),
             d, SLOT(rowsMoved(QModelIndex, int, int, QModelIndex, int)) );

    // save if data in model changed
    connect( m, SIGNAL(dataChanged(QModelIndex,QModelIndex)),
             SLOT(delayedSaveItems()) );
    connect( m, SIGNAL(rowsRemoved(QModelIndex,int,int)),
             SLOT(delayedSaveItems()) );
    connect( m, SIGNAL(rowsInserted(QModelIndex, int, int)),
             SLOT(delayedSaveItems()) );
    connect( m, SIGNAL(rowsMoved(QModelIndex, int, int, QModelIndex, int)),
             SLOT(delayedSaveItems()) );

    connect( this, SIGNAL(doubleClicked(QModelIndex)),
            SLOT(moveToClipboard(QModelIndex)));

    // ScrollPerItem doesn't work well with hidden items
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    setAttribute(Qt::WA_MacShowFocusRect, 0);
}

ClipboardBrowser::~ClipboardBrowser()
{
    emit closeAllEditors();
    if ( m_timerSave->isActive() )
        saveItems();
}


void ClipboardBrowser::closeExternalEditor(ItemEditor *editor)
{
    // check if file was modified before closing
    if ( editor->fileModified() )
        itemModified( editor->getText() );

    editor->disconnect(this);
    disconnect(editor);
    delete editor;
}

void ClipboardBrowser::contextMenuAction()
{
    QAction *act = qobject_cast<QAction *>(sender());
    Q_ASSERT(act != NULL);

    QVariant actionData = act->data();
    Q_ASSERT( actionData.isValid() );

    int i = actionData.toInt();
    if (i < 0 || i >= m_sharedData->commands.size())
        return;

    Command cmd = m_sharedData->commands[i];
    if ( cmd.outputTab.isEmpty() )
        cmd.outputTab = m_id;

    const QMimeData *data = (act->parent() == m_menu) ? getSelectedItemData() : clipboardData();
    if (data != NULL) {
        emit requestActionDialog(*data, cmd);
    } else {
        QMimeData textData;
        textData.setText(selectedText());
        emit requestActionDialog(textData, cmd);
    }
}

void ClipboardBrowser::createContextMenu()
{
    QAction *act;

    m_menu->clear();

    act = m_menu->addAction( iconClipboard(), tr("Move to &Clipboard") );
    m_menu->setDefaultAction(act);
    connect(act, SIGNAL(triggered()), this, SLOT(moveToClipboard()));

    act = m_menu->addAction( iconShowContent(), tr("&Show Content...") );
    act->setShortcut( QString("F4") );
    connect(act, SIGNAL(triggered()), this, SLOT(showItemContent()));

    act = m_menu->addAction( iconRemove(), tr("&Remove") );
    act->setShortcut( QString("Delete") );
    connect(act, SIGNAL(triggered()), this, SLOT(remove()));

    act = m_menu->addAction( iconEdit(), tr("&Edit") );
    act->setShortcut( QString("F2") );
    connect(act, SIGNAL(triggered()), this, SLOT(editSelected()));

    act = m_menu->addAction( iconEditExternal(), tr("E&dit with editor") );
    act->setShortcut( QString("Ctrl+E") );
    connect(act, SIGNAL(triggered()), this, SLOT(openEditor()));

    act = m_menu->addAction( iconAction(), tr("&Action...") );
    act->setShortcut( QString("F5") );
    connect(act, SIGNAL(triggered()), this, SLOT(action()));

    connect( m_menu, SIGNAL(aboutToShow()),
             this, SLOT(updateContextMenu()),
             Qt::UniqueConnection );
}

bool ClipboardBrowser::isFiltered(int row) const
{
    QString text = m->data(m->index(row), Qt::EditRole).toString();
    return m_lastFilter.indexIn(text) == -1;
}

void ClipboardBrowser::updateScrollOffset(const QModelIndex &index, int oldSize)
{
    int scrollOffset = verticalScrollBar()->value();
    if (scrollOffset > 0 && isIndexHidden(index))
        return;

    const QRect itemRect = rectForIndex(index);
    if ( scrollOffset > itemRect.y() ) {
        int dy = itemRect.height();
        if (oldSize <= 0)
            dy += 2 * spacing();
        else
            dy -= oldSize;

        // Negative oldSize means that item was removed.
        if (oldSize < 0)
            dy = -dy;

        verticalScrollBar()->setValue(scrollOffset + dy);
    }
}

bool ClipboardBrowser::fetchCacheForIndex(const QModelIndex &index)
{
    if ( editing() || d->hasCache(index) )
        return false;

    const int oldSize = sizeHintForIndex(index).height();
    d->cache(index);
    updateScrollOffset(index, oldSize);

    return true;
}

void ClipboardBrowser::addCommandsToMenu(QMenu *menu, QAction *insertBefore, const QString &text,
                                         const QMimeData *data)
{
    QAction *act;

    if ( m_sharedData->commands.isEmpty() )
        return;

    const QString windowTitle = data == NULL ? QString() : QString::fromUtf8(
                data->data("application/x-copyq-owner-window-title").data() );

    int i = -1;
    foreach (const Command &command, m_sharedData->commands) {
        ++i;

        // Verify that named command is provided and text is matched.
        if ( command.cmd.isEmpty())
            continue;
        if (command.name.isEmpty())
            continue;
        if (command.re.indexIn(text) == -1)
            continue;
        if (command.wndre.indexIn(windowTitle) == -1)
            continue;

        // Verify that data for given MIME is available.
        if ( !command.input.isEmpty() ) {
            if (data != NULL) {
                if ( !data->hasFormat(command.input) )
                    continue;
            } else if ( command.input != QString("text/plain") ) {
                continue;
            }
        }

        act = menu->addAction( IconFactory::iconFromFile(command.icon), command.name );
        act->setData( QVariant(i) );
        if ( !command.shortcut.isEmpty() )
            act->setShortcut( command.shortcut );

        menu->insertAction( insertBefore, act );
        insertBefore = act;

        elideText(act);

        connect(act, SIGNAL(triggered()), this, SLOT(contextMenuAction()));
    }
}

void ClipboardBrowser::updateContextMenu()
{
    QList<QAction *> actions = m_menu->actions();

    // remove old actions
    int i, len;
    for( i = 0, len = actions.size(); i<len && !actions[i]->isSeparator(); ++i ) {}
    for( ; i<len; ++i )
        m_menu->removeAction(actions[i]);

    m_menu->addSeparator();

    addCommandsToMenu(m_menu, NULL, selectedText(), getSelectedItemData());
}

void ClipboardBrowser::onRowChanged(int row, const QSize &oldSize)
{
    doItemsLayout();
    updateScrollOffset( index(row), oldSize.height() );
}

void ClipboardBrowser::contextMenuEvent(QContextMenuEvent *event)
{
    if ( !selectedIndexes().isEmpty() ) {
        m_menu->exec( event->globalPos() );
        event->accept();
    }
}

void ClipboardBrowser::paintEvent(QPaintEvent *event)
{
    // Stop caching after elapsed time and at least one newly cached item.
    static const qint64 maxElapsedMs = 100;
    QElapsedTimer timer;
    timer.start();

    QRect cacheRect = event->rect();

    // Pre-cache items on current and following page.
    cacheRect.setHeight( cacheRect.height() * 2 );

    QModelIndex ind;
    int i = 0;

    // Find first index to render.
    forever {
        ind = index(i);
        if ( !ind.isValid() )
            return;

        if ( !isIndexHidden(ind) && visualRect(ind).intersects(cacheRect) )
            break;

        d->hideRow(i);

        ++i;
    }

    // Render visible items.
    forever {
        if ( fetchCacheForIndex(ind) && timer.hasExpired(maxElapsedMs) )
            break;

        d->updateRowPosition( i, visualRect(ind).topLeft() );

        for ( ind = index(++i); ind.isValid() && isIndexHidden(ind); ind = index(++i) ) {}

        if ( !ind.isValid() )
            break;

        if ( !visualRect(ind).intersects(cacheRect) )
            break;
    }

    QListView::paintEvent(event);
}

void ClipboardBrowser::dataChanged(const QModelIndex &a, const QModelIndex &b)
{
    QListView::dataChanged(a, b);
    if ( autoUpdate() && a.row() == 0 )
        updateClipboard();
    d->dataChanged(a, b);
}

void ClipboardBrowser::resizeEvent(QResizeEvent *event)
{
    QListView::resizeEvent(event);
    if (m_sharedData->textWrap)
        d->setItemMaximumSize( viewport()->contentsRect().size() );
}

void ClipboardBrowser::commitData(QWidget *editor)
{
    QAbstractItemView::commitData(editor);
    saveItems();
}

bool ClipboardBrowser::openEditor()
{
    return openEditor( selectedText() );
}

bool ClipboardBrowser::openEditor(const QString &text)
{
    if ( m_sharedData->editor.isEmpty() )
        return false;

    ItemEditor *editor = new ItemEditor(text, m_sharedData->editor);

    connect( editor, SIGNAL(fileModified(const QString &)),
            this, SLOT(itemModified(const QString &)) );

    connect( editor, SIGNAL(closed(ItemEditor *)),
            this, SLOT(closeExternalEditor(ItemEditor *)) );

    connect( this, SIGNAL(closeAllEditors()), editor, SLOT(close()) );

    if ( !editor->start() ) {
        closeExternalEditor(editor);
        return false;
    }

    return true;
}

void ClipboardBrowser::addItems(const QStringList &items)
{
    for(int i=items.count()-1; i>=0; --i) {
        add(items[i], true);
    }
}

void ClipboardBrowser::showItemContent()
{
    const QMimeData *data = itemData();
    if (data == NULL)
        return;

    ClipboardDialog *d = new ClipboardDialog(data, this);
    connect( d, SIGNAL(finished(int)), d, SLOT(deleteLater()) );
    d->show();
}

void ClipboardBrowser::removeRow(int row)
{
    if (row < 0 && row >= model()->rowCount())
        return;
    updateScrollOffset( index(row), -1 );
    model()->removeRow(row);
}

void ClipboardBrowser::action()
{
    const QMimeData *data = getSelectedItemData();
    if (data != NULL) {
        emit requestActionDialog(*data);
    } else {
        QMimeData textData;
        textData.setText(selectedText());
        emit requestActionDialog(textData);
    }
}

void ClipboardBrowser::itemModified(const QString &str)
{
    // add new item
    if ( !str.isEmpty() ) {
        add(str, true);
        saveItems();
    }
}

void ClipboardBrowser::filterItems(const QString &str)
{
    if (m_lastFilter.pattern() == str)
        return;
    m_lastFilter = QRegExp(str, Qt::CaseInsensitive);

    // if search string empty: all items visible
    d->setSearch(m_lastFilter);

    // hide filtered items
    reset();
    int first = -1;
    for(int i = 0; i < m->rowCount(); ++i) {
        if ( isFiltered(i) ) {
            setRowHidden(i, true);
            d->hideRow(i);
        } else if (first == -1) {
            first = i;
        }
    }
    // select first visible
    setCurrentIndex( index(first) );
}

void ClipboardBrowser::moveToClipboard()
{
    moveToClipboard( currentIndex() );
}

void ClipboardBrowser::moveToClipboard(const QModelIndex &ind)
{
    if ( ind.isValid() )
        moveToClipboard(ind.row());
}

void ClipboardBrowser::moveToClipboard(int i)
{
    m->move(i,0);
    if ( autoUpdate() )
        updateClipboard();
    scrollTo( currentIndex() );
}

void ClipboardBrowser::editNew(const QString &text)
{
    add(text, true);
    selectionModel()->clearSelection();
    setCurrent(0);
    edit( index(0) );
}

void ClipboardBrowser::keyPressEvent(QKeyEvent *event)
{
    // ignore any input if editing an item
    if ( editing() )
        return;

    // translate keys for vi mode
    if (ConfigurationManager::instance()->value("vi").toBool() && handleViKey(event))
        return;

    int key = event->key();
    Qt::KeyboardModifiers mods = event->modifiers();

    // CTRL
    if (mods == Qt::ControlModifier) {
        switch ( key ) {
        // move items
        case Qt::Key_Down:
        case Qt::Key_Up:
        case Qt::Key_End:
        case Qt::Key_Home:
            if ( autoUpdate() && m->moveItems(selectedIndexes(), key) )
                updateClipboard();
            scrollTo( currentIndex() );
            event->accept();
            break;

        // cycle formats
        case Qt::Key_Left:
        case Qt::Key_Right: {
            QModelIndex index = currentIndex();
            if ( index.isValid() ) {
                if (key == Qt::Key_Left)
                    d->previousItemLoader(index);
                else
                    d->nextItemLoader(index);
            }
            event->accept();
            break;
        }

        default:
            updateContextMenu();
            QListView::keyPressEvent(event);
            break;
        }
    }
    else {
        switch ( key ) {
        // This fixes few issues with default navigation and item selections.
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageDown:
        case Qt::Key_PageUp:
        case Qt::Key_Home:
        case Qt::Key_End: {
            event->accept();

            QModelIndex current = currentIndex();
            int row = current.row();
            int d;

            if (key == Qt::Key_PageDown || key == Qt::Key_PageUp) {
                d = (key == Qt::Key_PageDown) ? 1 : -1;
                const int h = viewport()->height();
                QRect rect = visualRect(current);

                if ( rect.height() > h && d < 0 ? rect.top() < 0 : rect.bottom() > h ) {
                    QScrollBar *v = verticalScrollBar();
                    v->setValue( v->value() + d * v->pageStep() );
                    break;
                }

                int maxY = verticalOffset() + (d > 0 ? h : 0);
                for ( int i = row + d; i >= 0 && i < model()->rowCount(); i += d ) {
                    QModelIndex ind = index(i);
                    if ( !isIndexHidden(ind) ) {
                        const QRect rect = rectForIndex(ind);
                        if ( d > 0 ? rect.y() >= maxY : rect.bottom() <= maxY ) {
                            if ( row == current.row() )
                                maxY += d * h - rect.height();
                            else
                                break;
                        }
                        row = i;
                    }
                }
            } else {
                if (key == Qt::Key_Up) {
                    --row;
                } else if (key == Qt::Key_Down) {
                    ++row;
                } else {
                    if (key == Qt::Key_End) {
                        row = model()->rowCount() - 1;
                        d = 1;
                    } else {
                        row = 0;
                        d = -1;
                    }

                    for ( ; row != current.row() && isRowHidden(row); row -= d ) {}
                }
            }

            setCurrent(row, false, mods == Qt::ShiftModifier);
            break;
        }

        default:
            // allow user defined shortcuts
            updateContextMenu();
            QListView::keyPressEvent(event);
            // search
            event->ignore();
            break;
        }
    }
}

void ClipboardBrowser::setCurrent(int row, bool cycle, bool selection)
{
    QModelIndex prev = currentIndex();
    int cur = prev.row();

    // direction
    int dir = cur < row ? 1 : -1;

    // select first visible
    int i = m->getRowNumber(row, cycle);
    cur = i;
    while ( isRowHidden(i) ) {
        i = m->getRowNumber(i+dir, cycle);
        if ( (!cycle && (i==0 || i==m->rowCount()-1)) || i == cur)
            break;
    }
    if ( isRowHidden(i) )
        return;

    QModelIndex ind = index(i);
    if (selection) {
        ClipboardBrowser::Lock lock(this);
        QItemSelectionModel *sel = selectionModel();
        for ( int j = prev.row(); j != i + dir; j += dir ) {
            QModelIndex ind = index(j);
            if ( !ind.isValid() )
                break;
            if ( isIndexHidden(ind) )
                continue;

            if ( sel->isSelected(ind) && sel->isSelected(prev) )
                sel->setCurrentIndex(currentIndex(), QItemSelectionModel::Deselect);
            sel->setCurrentIndex(ind, QItemSelectionModel::Select);
            prev = ind;
        }
    } else {
        setCurrentIndex(ind);
    }

    scrollTo(ind); // ensure visible
}

ClipboardItem *ClipboardBrowser::at(int row) const
{
    return m->at(row);
}

void ClipboardBrowser::editSelected()
{
    if ( selectedIndexes().size() > 1 ) {
        editNew( selectedText() );
    } else {
        QModelIndex ind = currentIndex();
        if ( ind.isValid() ) {
            scrollTo(ind, PositionAtTop);
            emit requestShow(this);
            edit(ind);
        }
    }
}

void ClipboardBrowser::remove()
{
    QModelIndexList list = selectedIndexes();
    if ( list.isEmpty() )
        return;

    QList<int> rows;
    rows.reserve( list.size() );

    foreach (const QModelIndex &index, list)
        rows.append( index.row() );

    qSort( rows.begin(), rows.end(), qGreater<int>() );

    foreach (int row, rows) {
        if ( !isRowHidden(row) )
            m->removeRow(row);
    }

    int current = rows.last();

    // select next
    setCurrent(current);

    if ( autoUpdate() && current == 0 )
        updateClipboard();
}

void ClipboardBrowser::clear()
{
    m->removeRows(0, m->rowCount());
}

bool ClipboardBrowser::select(uint item_hash)
{
    int row = m->findItem(item_hash);
    if (row < 0)
        return false;

    return m->move(row, 0);
}

void ClipboardBrowser::sortItems(const QModelIndexList &indexes)
{
    m->sortItems(indexes, &alphaSort);
}

void ClipboardBrowser::reverseItems(const QModelIndexList &indexes)
{
    m->sortItems(indexes, &reverseSort);
}

bool ClipboardBrowser::add(const QString &txt, bool force)
{
    QMimeData *data = new QMimeData;
    data->setText(txt);
    return add(data, force);
}

bool ClipboardBrowser::add(QMimeData *data, bool force, int row)
{
    if (!force) {
        // don't add if new data is same as first item
        if ( m->rowCount() > 0 && *m->at(0) == *data ) {
            delete data;
            return false;
        }

        // commands
        if (data->hasText()) {
            const QString text = data->text();
            const QString windowTitle = QString::fromUtf8(
                        data->data("application/x-copyq-owner-window-title").data() );
            foreach (const Command &c, m_sharedData->commands) {
                if (c.automatic || c.ignore || !c.tab.isEmpty()) {
                    if ( c.re.indexIn(text) != -1
                         && (windowTitle.isNull() || c.wndre.indexIn(windowTitle) != -1) )
                    {
                        if (c.automatic) {
                            Command cmd = c;
                            if ( cmd.outputTab.isEmpty() )
                                cmd.outputTab = m_id;
                            if ( cmd.input.isEmpty() || data->hasFormat(cmd.input) )
                                emit requestActionDialog(*data, cmd);
                        }
                        if (!c.tab.isEmpty())
                            emit addToTab(data, c.tab);
                        if (c.ignore) {
                            delete data;
                            return false;
                        }
                    }
                }
            }
        }
    }

    // create new item
    int newRow = qMax(0, qMin(row, m->rowCount()));
    m->insertRow(newRow);
    QModelIndex ind = index(newRow);
    m->setData(ind, data);

    // filter item
    if ( isFiltered(newRow) )
        setRowHidden(newRow, true);

    // list size limit
    if ( m->rowCount() > m_sharedData->maxItems )
        m->removeRow( m->rowCount() - 1 );

    delayedSaveItems();

    // Keep scroll offset.
    updateScrollOffset(ind, 0);

    return true;
}

bool ClipboardBrowser::add(const ClipboardItem &item, bool force, int row)
{
    return add( cloneData(*item.data()), force, row );
}

void ClipboardBrowser::loadSettings()
{
    ConfigurationManager *cm = ConfigurationManager::instance();

    cm->decorateBrowser(this);

    // restore configuration
    m->setMaxItems(m_sharedData->maxItems);

    setTextWrap(m_sharedData->textWrap);

    d->setSaveOnEnterKey(m_sharedData->saveOnReturnKey);

    // re-create menu
    createContextMenu();
}

void ClipboardBrowser::loadItems()
{
    if ( m_id.isEmpty() ) return;
    ConfigurationManager::instance()->loadItems(*m, m_id);
    m_timerSave->stop();
    setCurrentIndex( QModelIndex() );
}

void ClipboardBrowser::saveItems()
{
    if ( m_id.isEmpty() )
        return;

    m_timerSave->stop();

    ConfigurationManager::instance()->saveItems(*m, m_id);
}

void ClipboardBrowser::delayedSaveItems(int msec)
{
    if ( m_id.isEmpty() || m_timerSave->isActive() )
        return;

    m_timerSave->start(msec);
}

void ClipboardBrowser::purgeItems()
{
    if ( m_id.isEmpty() ) return;
    ConfigurationManager::instance()->removeItems(m_id);
    m_timerSave->stop();
}

const QString ClipboardBrowser::selectedText() const
{
    QString result;
    QItemSelectionModel *sel = selectionModel();

    QModelIndexList list = sel->selectedIndexes();
    foreach (const QModelIndex &ind, list) {
        if ( !isIndexHidden(ind) ) {
            if ( !result.isEmpty() )
                result += QString('\n');
            result += itemText(ind);
        }
    }

    return result;
}

QString ClipboardBrowser::itemText(int i) const
{
    if ( i >= m->rowCount() )
        return QString();
    return itemText( (i==-1) ? currentIndex() : index(i) );
}

QString ClipboardBrowser::itemText(QModelIndex ind) const
{
    return ind.isValid() ? ind.data(Qt::EditRole).toString() : QString();
}

const QMimeData *ClipboardBrowser::itemData(int i) const
{
    return m->mimeDataInRow( i>=0 ? i : currentIndex().row() );
}

void ClipboardBrowser::updateClipboard()
{
    if ( m->rowCount() > 0 )
        emit changeClipboard(m->at(0));
}

void ClipboardBrowser::redraw()
{
    d->invalidateCache();
    update();
}

bool ClipboardBrowser::editing()
{
    return state() == QAbstractItemView::EditingState;
}

bool ClipboardBrowser::handleViKey(QKeyEvent *event)
{
    bool handle = true;
    int key = event->key();
    Qt::KeyboardModifiers mods = event->modifiers();

    switch ( key ) {
    case Qt::Key_G:
        key = mods & Qt::ShiftModifier ? Qt::Key_End : Qt::Key_Home;
        mods = mods & ~Qt::ShiftModifier;
        break;
    case Qt::Key_J:
        key = Qt::Key_Down;
        break;
    case Qt::Key_K:
        key = Qt::Key_Up;
        break;
    default:
        handle = false;
    }

    if (!handle && mods & Qt::ControlModifier) {
        switch ( key ) {
        case Qt::Key_F:
        case Qt::Key_D:
            key = Qt::Key_PageDown;
            mods = mods & ~Qt::ControlModifier;
            handle = true;
            break;
        case Qt::Key_B:
        case Qt::Key_U:
            key = Qt::Key_PageUp;
            mods = mods & ~Qt::ControlModifier;
            handle = true;
            break;
        }
    }

    if (handle) {
        QKeyEvent event2(QEvent::KeyPress, key, mods, event->text());
        keyPressEvent(&event2);
        event->accept();
    }

    return handle;
}

void ClipboardBrowser::setTextWrap(bool enabled)
{
    d->setItemMaximumSize( enabled ? viewport()->contentsRect().size() : QSize(2048, 2048) );
}

const QMimeData *ClipboardBrowser::getSelectedItemData() const
{
    QModelIndexList selected = selectionModel()->selectedRows();
    return (selected.size() == 1) ? itemData(selected.first().row()) : NULL;
}
