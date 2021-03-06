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

#include "clipboarditem.h"

#include "clipboardmodel.h"

#include <QSize>

const QModelIndex emptyIndex;

ClipboardModel::ClipboardModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_clipboardList()
    , m_max(100)
{
}

ClipboardModel::~ClipboardModel()
{
    foreach (ClipboardItem *item, m_clipboardList)
        delete item;
}

int ClipboardModel::rowCount(const QModelIndex&) const
{
    return m_clipboardList.size();
}

QMimeData *ClipboardModel::mimeDataInRow(int row) const
{
    if (row < rowCount()) {
        return m_clipboardList[row]->data();
    } else {
        return NULL;
    }
}

ClipboardItem *ClipboardModel::at(int row) const
{
    return m_clipboardList[row];
}

QVariant ClipboardModel::data(int row) const
{
    return data( index(row,0), Qt::EditRole );
}

QVariant ClipboardModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_clipboardList.size())
        return QVariant();

    return m_clipboardList[index.row()]->data(role);
}

Qt::ItemFlags ClipboardModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::ItemIsEnabled;

    return QAbstractItemModel::flags(index) | Qt::ItemIsEditable;
}

bool ClipboardModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (index.isValid() && role == Qt::EditRole) {
        int row = index.row();
        m_clipboardList[row]->setData(value);
        emit dataChanged(index, index);
        return true;
    }
    return false;
}

bool ClipboardModel::setData(const QModelIndex &index, QMimeData *value)
{
    if (index.isValid()) {
        int row = index.row();
        m_clipboardList[row]->setData(value);
        emit dataChanged(index, index);
        return true;
    }
    return false;
}

ClipboardItem *ClipboardModel::append()
{
    int rows = rowCount();
    ClipboardItem *item = new ClipboardItem();
    beginInsertRows(emptyIndex, rows, rows);
    m_clipboardList.append(item);
    endInsertRows();
    return item;
}

bool ClipboardModel::insertRows(int position, int rows, const QModelIndex&)
{
    ClipboardItem *item;
    beginInsertRows(emptyIndex, position, position+rows-1);

    for (int row = 0; row < rows; ++row) {
        item = new ClipboardItem();
        m_clipboardList.insert(position, item);
    }

    endInsertRows();
    return true;
}

bool ClipboardModel::removeRows(int position, int rows, const QModelIndex&)
{
    int count = rowCount();
    if (position >= count)
        return false;

    int last = qMin(position+rows-1, count-1);
    beginRemoveRows(emptyIndex, position, last);

    for (int row = position; row <= last; ++row) {
        delete m_clipboardList.takeAt(position);
    }

    endRemoveRows();
    return true;
}

int ClipboardModel::getRowNumber(int row, bool cycle) const
{
    int n = rowCount();
    if (n == 0)
        return -1;
    if (row >= n)
        return cycle ? 0 : n-1;
    else if (row < 0)
        return cycle ? n-1 : 0;
    else
        return row;
}

void ClipboardModel::setMaxItems(int max)
{
    m_max = max>0 ? max : 0;
    int rows = m_clipboardList.length();

    if (max >= rows)
        return;

    // crop list
    beginRemoveRows(emptyIndex, max, rows-1 );

    while ( rows > max ) {
        delete m_clipboardList.takeLast();
        --rows;
    }

    endRemoveRows();
}

bool ClipboardModel::move(int pos, int newpos)
{
    int from = getRowNumber(pos,true);
    int to   = getRowNumber(newpos,true);

    if( from == -1 || to == -1 )
        return false;
    if ( !beginMoveRows(emptyIndex, from, from, emptyIndex,
                        from < to ? to+1 : to) )
        return false;
    m_clipboardList.move(from, to);
    endMoveRows();
    return true;
}

bool ClipboardModel::moveItems(QModelIndexList indexList, int key) {
    int from, to;
    bool res = false;

    QList<int> list;
    for ( int i = 0; i < indexList.length(); ++i )
        list.append( indexList.at(i).row() );

    if ( key == Qt::Key_Down || key == Qt::Key_End )
        qSort( list.begin(), list.end(), qGreater<int>() );
    else
        qSort( list.begin(), list.end(), qLess<int>() );

    for ( int i = 0, d = 0; i<list.length(); ++i ) {
        from = list.at(i) + d;

        switch (key) {
        case Qt::Key_Down:
            to = from+1;
            break;
        case Qt::Key_Up:
            to = from-1;
            break;
        case Qt::Key_End:
            to = rowCount()-i-1;
            break;
        default:
            to = 0+i;
            break;
        }

        if ( to < 0 )
            --d;
        else if (to >= rowCount() )
            ++d;

        if ( !move(from, to) )
            return false;
        if (!res)
            res = to==0 || from==0 || to == rowCount();
    }

    return res;
}

void ClipboardModel::sortItems(const QModelIndexList &indexList,
                               CompareItems *compare)
{
    ComparisonItem a, b;
    QList< QPair<int, ClipboardItem*> > list;
    QList<int> rows;

    for (int i = 0; i < indexList.length(); ++i) {
        int row = indexList[i].row();
        if ( row >= m_clipboardList.length() )
            return;
        list.append( qMakePair(row, m_clipboardList[row]) );
        rows.append(row);
    }

    qSort(rows);
    qSort( list.begin(), list.end(), compare );

    for (int i = 0; i < list.length(); ++i ) {
        int row1 = list[i].first;
        int row2 = rows[i];
        if (row1 != row2) {
            m_clipboardList[row2] = list[i].second;
            QModelIndex ind = index(row2);
            emit dataChanged(ind, ind);
        }
    }
}

int ClipboardModel::findItem(uint item_hash) const
{
    for (int i = 0; i < m_clipboardList.length(); ++i) {
        if ( m_clipboardList[i]->dataHash() == item_hash )
            return i;
    }
    return -1;
}

QDataStream &operator<<(QDataStream &stream, const ClipboardModel &model)
{
    int length = model.rowCount();
    stream << length;

    // save in reverse order so the items
    // can be later restored in right order
    for(int i = 0; i < length; ++i)
        stream << *model.at(i);

    return stream;
}

QDataStream &operator>>(QDataStream &stream, ClipboardModel &model)
{
    int length;
    stream >> length;
    length = qMin( length, model.maxItems() ) - model.rowCount();

    ClipboardItem *item;
    for(int i = 0; i < length; ++i) {
        item = model.append();
        stream >> *item;
    }

    return stream;
}
