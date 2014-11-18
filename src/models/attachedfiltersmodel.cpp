/*
 * Copyright (c) 2013 Meltytech, LLC
 * Author: Dan Dennedy <dan@dennedy.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "attachedfiltersmodel.h"
#include "mltcontroller.h"
#include "mainwindow.h"
#include "controllers/filtercontroller.h"
#include "qmltypes/qmlmetadata.h"
#include <QTimer>
#include <QDebug>

static bool sortIsLess (const QmlMetadata* lhs, const QmlMetadata* rhs) {
    // Sort order is: GPU, Video, Audio
    // If metadata is NULL, assume it is video.
    if (!lhs && !rhs) {
        return false;
    } else if (!lhs) {
        if (rhs->needsGPU()) {
            return true;
        }
    } else if (!rhs) {
        if (lhs->isAudio()) {
            return true;
        }
    } else if (rhs->needsGPU() && !lhs->needsGPU()) {
        return true;
    } else if (!rhs->isAudio() && lhs->isAudio()) {
        return true;
    }
    return false;
}

AttachedFiltersModel::AttachedFiltersModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_dropRow(-1)
{
}

bool AttachedFiltersModel::isReady()
{
    return m_producer != NULL;
}

Mlt::Filter* AttachedFiltersModel::getFilter(int row) const
{
    Mlt::Filter* result = 0;
    if (m_producer && m_producer->is_valid() && row < m_mltIndexMap.count()) {
        result = m_producer->filter(m_mltIndexMap[row]);
    }
    return result;
}

const QmlMetadata* AttachedFiltersModel::getMetadata(int row) const
{
    if (row < m_metaList.count()) {
        return m_metaList[row];
    }
    return NULL;
}

int AttachedFiltersModel::rowCount(const QModelIndex &parent) const
{
    if (m_producer && m_producer->is_valid())
        return m_metaList.count();
    else
        return 0;
}

Qt::ItemFlags AttachedFiltersModel::flags(const QModelIndex &index) const
{
    if (index.isValid())
        return QAbstractListModel::flags(index) | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled;
    else
        return QAbstractListModel::flags(index) | Qt::ItemIsDropEnabled;
}

QVariant AttachedFiltersModel::data(const QModelIndex &index, int role) const
{
    if ( !m_producer || !m_producer->is_valid()
        || index.row() >= m_producer->filter_count())
        return QVariant();
    switch (role ) {
    case Qt::DisplayRole: {
            QVariant result;
            const QmlMetadata* meta = m_metaList[index.row()];
            if (meta) {
                result = meta->name();
            } else {
                // Fallback is raw mlt_service name
                Mlt::Filter* filter = getFilter(index.row());
                if (filter && filter->is_valid() && filter->get("mlt_service")) {
                    result = QString::fromUtf8(filter->get("mlt_service"));
                }
                delete filter;
            }
            return result;
        }
    case Qt::CheckStateRole: {
            Mlt::Filter* filter = getFilter(index.row());
            QVariant result = Qt::Unchecked;
            if (filter && filter->is_valid() && !filter->get_int("disable"))
                result = Qt::Checked;
            delete filter;
            return result;
        }
        break;
    case TypeDisplayRole: {
            QVariant result;
            const QmlMetadata* meta = m_metaList[index.row()];
            if (meta && meta->isAudio()) {
                result = tr("Audio");
            } else if (meta && meta->needsGPU()) {
                result = tr("GPU");
            } else {
                result = tr("Video");
            }
            return result;
        }
        break;
    default:
        break;
    }
    return QVariant();
}

bool AttachedFiltersModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (role == Qt::CheckStateRole) {
        Mlt::Filter* filter = getFilter(index.row());
        if (filter && filter->is_valid()) {
            filter->set("disable", !filter->get_int("disable"));
            emit changed();
            emit dataChanged(createIndex(index.row(), 0), createIndex(index.row(), 0));
        }
        delete filter;
        return true;
    }
    return false;
}

QHash<int, QByteArray> AttachedFiltersModel::roleNames() const {
    QHash<int, QByteArray> roles = QAbstractListModel::roleNames();
    roles[Qt::CheckStateRole] = "checkState";
    roles[TypeDisplayRole] = "typeDisplay";
    return roles;
}

Qt::DropActions AttachedFiltersModel::supportedDropActions() const
{
    return Qt::MoveAction;
}

bool AttachedFiltersModel::insertRows(int row, int count, const QModelIndex &parent)
{
    if (m_producer && m_producer->is_valid()) {
        if (m_dropRow == -1)
            m_dropRow = row;
        return true;
    } else {
        return false;
    }
}

bool AttachedFiltersModel::removeRows(int row, int count, const QModelIndex &parent)
{
    if (m_producer && m_producer->is_valid() && m_dropRow >= 0 && row != m_dropRow) {
        bool result = moveRows(parent, row, 1, parent, m_dropRow);
        m_dropRow = -1;
        return result;
    } else {
        return false;
    }
}

bool AttachedFiltersModel::moveRows(const QModelIndex & sourceParent, int sourceRow, int count, const QModelIndex & destinationParent, int destinationRow)
{
    if (!m_producer || !m_producer->is_valid() || sourceParent != destinationParent || count != 1) {
        return false;
    }

    QModelIndex fromIndex = createIndex(sourceRow, 0);
    QModelIndex toIndex = createIndex(destinationRow, 0);

    if (fromIndex.isValid() && toIndex.isValid()) {
        if (beginMoveRows(sourceParent, sourceRow, sourceRow, destinationParent, destinationRow)) {
            if (destinationRow > sourceRow) {
                // Moving down: Convert to "post move" indexing
                destinationRow--;
            }
            int mltSrcIndex = m_mltIndexMap[sourceRow];
            int mltDstIndex = m_mltIndexMap[destinationRow];
            m_producer->move_filter(mltSrcIndex, mltDstIndex);
            // Adjust MLT index map for indices that just changed.
            m_mltIndexMap.removeAt(sourceRow);
            for (int i = 0; i < m_mltIndexMap.count(); i++) {
                if (m_mltIndexMap[i] > mltSrcIndex) {
                    m_mltIndexMap[i] = m_mltIndexMap[i] - 1;
                }
                if (m_mltIndexMap[i] >= mltDstIndex) {
                    m_mltIndexMap[i] = m_mltIndexMap[i] + 1;
                }
            }
            m_mltIndexMap.insert(destinationRow, mltDstIndex);
            m_metaList.move(sourceRow, destinationRow);
            endMoveRows();
            return true;
        }
    }
    return false;
}

int AttachedFiltersModel::add(const QmlMetadata* meta)
{
    int insertIndex = -1;
    int mltIndex = -1;
    Mlt::Filter* filter = new Mlt::Filter(MLT.profile(), meta->mlt_service().toUtf8().constData());
    if (filter->is_valid()) {
        if (!meta->objectName().isEmpty())
            filter->set("shotcut:filter", meta->objectName().toUtf8().constData());

        // Put the filter after the last filter that is greater than or equal
        // in sort order.
        insertIndex = 0;
        for (int i = m_metaList.count() - 1; i >= 0; i--) {
            if (!sortIsLess(m_metaList[i], meta)) {
                insertIndex = i + 1;
                break;
            }
        }

        // Calculate the MLT index for the new filter.
        if (m_mltIndexMap.count() == 0) {
            mltIndex = m_mltIndexMap.count();
        } else if (insertIndex == 0) {
            mltIndex = m_mltIndexMap[0];
        } else {
            mltIndex = m_mltIndexMap[insertIndex -1] + 1;
        }

        beginInsertRows(QModelIndex(), insertIndex, insertIndex);
        MLT.pause();
        m_producer->attach(*filter);
        m_producer->move_filter(m_producer->filter_count() - 1, mltIndex);
        // Adjust MLT index map for indices that just changed.
        for (int i = 0; i < m_mltIndexMap.count(); i++) {
            if (m_mltIndexMap[i] >= mltIndex) {
                m_mltIndexMap[i] = m_mltIndexMap[i] + 1;
            }
        }
        m_mltIndexMap.insert(insertIndex, mltIndex);
        m_metaList.insert(insertIndex, meta);
        endInsertRows();
        emit changed();
    }
    else qWarning() << "Failed to load filter" << meta->mlt_service();
    delete filter;
    return insertIndex;
}

void AttachedFiltersModel::remove(int row)
{
    if (row >= m_metaList.count()) {
        qWarning() << "Invalid index:" << row;
        return;
    }

    beginRemoveRows(QModelIndex(), row, row);
    int mltIndex = m_mltIndexMap[row];
    Mlt::Filter* filter = m_producer->filter(mltIndex);
    m_producer->detach(*filter);
    // Adjust MLT index map for indices that just changed.
    m_mltIndexMap.removeAt(row);
    for (int i = 0; i < m_mltIndexMap.count(); i++) {
        if (m_mltIndexMap[i] > mltIndex) {
            m_mltIndexMap[i] = m_mltIndexMap[i] - 1;
        }
    }
    m_metaList.removeAt(row);
    endRemoveRows();
    emit changed();
    delete filter;
}

bool AttachedFiltersModel::move(int fromRow, int toRow)
{
    QModelIndex parent = QModelIndex();

    if (fromRow < 0 || toRow < 0) {
        return false;
    }

    if (toRow > fromRow) {
        // Moving down: put it under the destination index
        toRow++;
    }

    return moveRows(parent, fromRow, 1, parent, toRow);
}

void AttachedFiltersModel::reset(Mlt::Producer* producer)
{
    beginResetModel();

    m_producer.reset(new Mlt::Producer(producer ? producer : MLT.producer()));
    m_metaList.clear();
    m_mltIndexMap.clear();

    if (MLT.isPlaylist()) return;
    if (m_producer && m_producer->is_valid()) {
        int count = m_producer->filter_count();
        for (int i = 0; i < count; i++) {
            Mlt::Filter* filter = m_producer->filter(i);
            if (filter && filter->is_valid() && !filter->get_int("_loader")) {
                QmlMetadata* newMeta = MAIN.filterController()->metadataForService(filter);
                int newIndex = m_metaList.count();
                for (int j = newIndex - 1; j >= 0; j--) {
                    const QmlMetadata* prevMeta = m_metaList[j];
                    if (sortIsLess(prevMeta, newMeta)) {
                        newIndex = j;
                    } else {
                        break;
                    }
                }
                m_metaList.insert(newIndex, newMeta);
                m_mltIndexMap.insert(newIndex, i);
            }
            delete filter;
        }
    }

    endResetModel();
    emit readyChanged();
}
