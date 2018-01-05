/*
Copyright (C) 2017  Jean-Baptiste Mardelle <jb@kdenlive.org>
This file is part of Kdenlive. See www.kdenlive.org.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of
the License or (at your option) version 3 or any later version
accepted by the membership of KDE e.V. (or its successor approved
by the membership of KDE e.V.), which shall act as a proxy
defined in Section 14 of version 3 of the license.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "timelinefunctions.hpp"
#include "clipmodel.hpp"
#include "compositionmodel.hpp"
#include "core.h"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "groupsmodel.hpp"
#include "timelineitemmodel.hpp"
#include "trackmodel.hpp"

#include <QDebug>
#include <klocalizedstring.h>

bool TimelineFunctions::copyClip(std::shared_ptr<TimelineItemModel> timeline, int clipId, int &newId, PlaylistState::ClipState state, Fun &undo, Fun &redo)
{
    bool res = timeline->requestClipCreation(timeline->getClipBinId(clipId), newId, state, undo, redo);
    timeline->m_allClips[newId]->m_endlessResize = timeline->m_allClips[clipId]->m_endlessResize;
    // copy useful timeline properties
    timeline->m_allClips[clipId]->passTimelineProperties(timeline->m_allClips[newId]);

    int duration = timeline->getClipPlaytime(clipId);
    int init_duration = timeline->getClipPlaytime(newId);
    if (duration != init_duration) {
        int in = timeline->m_allClips[clipId]->getIn();
        res = res && timeline->requestItemResize(newId, init_duration - in, false, true, undo, redo);
        res = res && timeline->requestItemResize(newId, duration, true, true, undo, redo);
    }
    if (!res) {
        return false;
    }
    std::shared_ptr<EffectStackModel> sourceStack = timeline->getClipEffectStackModel(clipId);
    std::shared_ptr<EffectStackModel> destStack = timeline->getClipEffectStackModel(newId);
    destStack->importEffects(sourceStack);
    return res;
}

bool TimelineFunctions::requestClipCut(std::shared_ptr<TimelineItemModel> timeline, int clipId, int position, int &newId, Fun &undo, Fun &redo)
{
    int start = timeline->getClipPosition(clipId);
    int duration = timeline->getClipPlaytime(clipId);
    if (start > position || (start + duration) < position) {
        return false;
    }
    PlaylistState::ClipState state = timeline->m_allClips[clipId]->clipState();
    bool res = copyClip(timeline, clipId, newId, state, undo, redo);
    res = res && timeline->requestItemResize(clipId, position - start, true, true, undo, redo);
    int newDuration = timeline->getClipPlaytime(clipId);
    res = res && timeline->requestItemResize(newId, duration - newDuration, false, true, undo, redo);
    res = res && timeline->requestClipMove(newId, timeline->getClipTrackId(clipId), position, true, false, undo, redo);
    return res;
}

bool TimelineFunctions::requestClipCut(std::shared_ptr<TimelineItemModel> timeline, int clipId, int position)
{
    std::function<bool(void)> undo = []() { return true; };
    std::function<bool(void)> redo = []() { return true; };
    const std::unordered_set<int> clips = timeline->getGroupElements(clipId);
    int count = 0;
    for (int cid : clips) {
        int start = timeline->getClipPosition(cid);
        int duration = timeline->getClipPlaytime(cid);
        if (start < position && (start + duration) > position) {
            count++;
            int newId;
            bool res = requestClipCut(timeline, cid, position, newId, undo, redo);
            if (!res) {
                bool undone = undo();
                Q_ASSERT(undone);
                return false;
            }
            // splitted elements go temporarily in the same group as original ones.
            timeline->m_groups->setInGroupOf(newId, cid, undo, redo);
        }
    }
    if (count > 0 && timeline->m_groups->isInGroup(clipId)) {
        // we now split the group hiearchy.
        // As a splitting criterion, we compare start point with split position
        auto criterion = [timeline, position](int cid) { return timeline->getClipPosition(cid) < position; };
        int root = timeline->m_groups->getRootId(clipId);
        bool res = timeline->m_groups->split(root, criterion, undo, redo);
        if (!res) {
            bool undone = undo();
            Q_ASSERT(undone);
            return false;
        }
    }
    if (count > 0) {
        pCore->pushUndo(undo, redo, i18n("Cut clip"));
    }
    return count > 0;
}

int TimelineFunctions::requestSpacerStartOperation(std::shared_ptr<TimelineItemModel> timeline, int trackId, int position)
{
    std::unordered_set<int> clips = timeline->getItemsAfterPosition(trackId, position, -1);
    if (clips.size() > 0) {
        timeline->requestClipsGroup(clips, false);
        return (*clips.cbegin());
    }
    return -1;
}

bool TimelineFunctions::requestSpacerEndOperation(std::shared_ptr<TimelineItemModel> timeline, int clipId, int startPosition, int endPosition)
{
    // Move group back to original position
    int track = timeline->getItemTrackId(clipId);
    timeline->requestClipMove(clipId, track, startPosition, false, false);
    std::unordered_set<int> clips = timeline->getGroupElements(clipId);
    // break group
    timeline->requestClipUngroup(clipId, false);
    // Start undoable command
    std::function<bool(void)> undo = []() { return true; };
    std::function<bool(void)> redo = []() { return true; };
    int res = timeline->requestClipsGroup(clips, undo, redo);
    bool final = false;
    if (res > -1) {
        if (clips.size() > 1) {
            final = timeline->requestGroupMove(clipId, res, 0, endPosition - startPosition, true, true, undo, redo);
        } else {
            // only 1 clip to be moved
            final = timeline->requestClipMove(clipId, track, endPosition, true, true, undo, redo);
        }
    }
    if (final && clips.size() > 1) {
        final = timeline->requestClipUngroup(clipId, undo, redo);
    }
    if (final) {
        pCore->pushUndo(undo, redo, i18n("Insert space"));
        return true;
    }
    return false;
}

bool TimelineFunctions::extractZone(std::shared_ptr<TimelineItemModel> timeline, QVector<int> tracks, QPoint zone, bool liftOnly)
{
    // Start undoable command
    std::function<bool(void)> undo = []() { return true; };
    std::function<bool(void)> redo = []() { return true; };
    bool result = false;
    for (int trackId : tracks) {
        result = TimelineFunctions::liftZone(timeline, trackId, zone, undo, redo);
        if (result && !liftOnly) {
            result = TimelineFunctions::removeSpace(timeline, trackId, zone, undo, redo);
        }
    }
    pCore->pushUndo(undo, redo, liftOnly ? i18n("Lift zone") : i18n("Extract zone"));
    return result;
}

bool TimelineFunctions::insertZone(std::shared_ptr<TimelineItemModel> timeline, int trackId, const QString &binId, int insertFrame, QPoint zone, bool overwrite)
{
    // Start undoable command
    std::function<bool(void)> undo = []() { return true; };
    std::function<bool(void)> redo = []() { return true; };
    bool result = false;
    if (overwrite) {
        result = TimelineFunctions::liftZone(timeline, trackId, QPoint(insertFrame, insertFrame + (zone.y() - zone.x())), undo, redo);
    } else {
        int startClipId = timeline->getClipByPosition(trackId, insertFrame);
        int startCutId = -1;
        if (startClipId > -1) {
            // There is a clip, cut it
            TimelineFunctions::requestClipCut(timeline, startClipId, insertFrame, startCutId, undo, redo);
        }
        result = TimelineFunctions::insertSpace(timeline, trackId, QPoint(insertFrame, insertFrame + (zone.y() - zone.x())), undo, redo);
    }
    int newId = -1;
    QString binClipId = QString("%1#%2#%3").arg(binId).arg(zone.x()).arg(zone.y() - 1);
    timeline->requestClipInsertion(binClipId, trackId, insertFrame, newId, true, true, undo, redo);
    pCore->pushUndo(undo, redo, overwrite ? i18n("Overwrite zone") : i18n("Insert zone"));
    return result;
}

bool TimelineFunctions::liftZone(std::shared_ptr<TimelineItemModel> timeline, int trackId, QPoint zone, Fun &undo, Fun &redo)
{
    // Check if there is a clip at start point
    int startClipId = timeline->getClipByPosition(trackId, zone.x());
    int startCutId = -1;
    if (startClipId > -1) {
        // There is a clip, cut it
        TimelineFunctions::requestClipCut(timeline, startClipId, zone.x(), startCutId, undo, redo);
    }
    int endCutId = -1;
    int endClipId = timeline->getClipByPosition(trackId, zone.y());
    if (endClipId > -1) {
        // There is a clip, cut it
        TimelineFunctions::requestClipCut(timeline, endClipId, zone.y(), endCutId, undo, redo);
    }
    std::unordered_set<int> clips = timeline->getItemsAfterPosition(trackId, zone.x(), zone.y() - 1);
    for (const auto &clipId : clips) {
        timeline->requestClipDeletion(clipId, undo, redo);
    }
    return true;
}

bool TimelineFunctions::removeSpace(std::shared_ptr<TimelineItemModel> timeline, int trackId, QPoint zone, Fun &undo, Fun &redo)
{
    std::unordered_set<int> clips = timeline->getItemsAfterPosition(-1, zone.y() - 1, -1, true);
    bool result = false;
    if (clips.size() > 0) {
        int clipId = *clips.begin();
        if (clips.size() > 1) {
            int res = timeline->requestClipsGroup(clips, undo, redo);
            if (res > -1) {
                result = timeline->requestGroupMove(clipId, res, 0, zone.x() - zone.y(), true, true, undo, redo);
                if (result) {
                    result = timeline->requestClipUngroup(clipId, undo, redo);
                }
            }
        } else {
            // only 1 clip to be moved
            int clipStart = timeline->getItemPosition(clipId);
            result = timeline->requestClipMove(clipId, timeline->getItemTrackId(clipId), clipStart - (zone.y() - zone.x()), true, true, undo, redo);
        }
    }
    return result;
}

bool TimelineFunctions::insertSpace(std::shared_ptr<TimelineItemModel> timeline, int trackId, QPoint zone, Fun &undo, Fun &redo)
{
    std::unordered_set<int> clips = timeline->getItemsAfterPosition(-1, zone.x(), -1, true);
    bool result = false;
    if (clips.size() > 0) {
        int clipId = *clips.begin();
        if (clips.size() > 1) {
            int res = timeline->requestClipsGroup(clips, undo, redo);
            if (res > -1) {
                result = timeline->requestGroupMove(clipId, res, 0, zone.y() - zone.x(), true, true, undo, redo);
                if (result) {
                    result = timeline->requestClipUngroup(clipId, undo, redo);
                }
            }
        } else {
            // only 1 clip to be moved
            int clipStart = timeline->getItemPosition(clipId);
            result = timeline->requestClipMove(clipId, timeline->getItemTrackId(clipId), clipStart + (zone.y() - zone.x()), true, true, undo, redo);
        }
    }
    return result;
}

bool TimelineFunctions::requestClipCopy(std::shared_ptr<TimelineItemModel> timeline, int clipId, int trackId, int position)
{
    Q_ASSERT(timeline->isClip(clipId) || timeline->isComposition(clipId));
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    int deltaTrack = timeline->getTrackPosition(trackId) - timeline->getTrackPosition(timeline->getClipTrackId(clipId));
    int deltaPos = position - timeline->getClipPosition(clipId);
    std::unordered_set<int> allIds = timeline->getGroupElements(clipId);
    std::unordered_map<int, int> mapping; // keys are ids of the source clips, values are ids of the copied clips
    for (int id : allIds) {
        int newId = -1;
        PlaylistState::ClipState state = timeline->m_allClips[id]->clipState();
        bool res = copyClip(timeline, id, newId, state, undo, redo);
        res = res && (newId != -1);
        int target_position = timeline->getClipPosition(id) + deltaPos;
        int target_track_position = timeline->getTrackPosition(timeline->getClipTrackId(id)) + deltaTrack;
        if (target_track_position >= 0 && target_track_position < timeline->getTracksCount()) {
            auto it = timeline->m_allTracks.cbegin();
            std::advance(it, target_track_position);
            int target_track = (*it)->getId();
            res = res && timeline->requestClipMove(newId, target_track, target_position, true, false, undo, redo);
        } else {
            res = false;
        }
        if (!res) {
            bool undone = undo();
            Q_ASSERT(undone);
            return false;
        }
        mapping[id] = newId;
    }
    qDebug() << "Sucessful copy, coping groups...";
    bool res = timeline->m_groups->copyGroups(mapping, undo, redo);
    if (!res) {
        bool undone = undo();
        Q_ASSERT(undone);
        return false;
    }
    return true;
}

void TimelineFunctions::showClipKeyframes(std::shared_ptr<TimelineItemModel> timeline, int clipId, bool value)
{
    timeline->m_allClips[clipId]->setShowKeyframes(value);
    QModelIndex modelIndex = timeline->makeClipIndexFromID(clipId);
    timeline->dataChanged(modelIndex, modelIndex, {TimelineModel::KeyframesRole});
}

void TimelineFunctions::showCompositionKeyframes(std::shared_ptr<TimelineItemModel> timeline, int compoId, bool value)
{
    timeline->m_allCompositions[compoId]->setShowKeyframes(value);
    QModelIndex modelIndex = timeline->makeCompositionIndexFromID(compoId);
    timeline->dataChanged(modelIndex, modelIndex, {TimelineModel::KeyframesRole});
}

bool TimelineFunctions::changeClipState(std::shared_ptr<TimelineItemModel> timeline, int clipId, PlaylistState::ClipState status)
{
    PlaylistState::ClipState oldState = timeline->m_allClips[clipId]->clipState();
    if (oldState == status) {
        return false;
    }
    std::function<bool(void)> undo = []() { return true; };
    std::function<bool(void)> redo = []() { return true; };
    redo = [timeline, clipId, status]() {
        int trackId = timeline->getClipTrackId(clipId);
        bool res = timeline->m_allClips[clipId]->setClipState(status);
        // in order to make the producer change effective, we need to unplant / replant the clip in int track
        if (trackId != -1) {
            timeline->getTrackById(trackId)->replugClip(clipId);
            QModelIndex ix = timeline->makeClipIndexFromID(clipId);
            timeline->dataChanged(ix, ix, {TimelineModel::StatusRole});
            timeline->invalidateClip(clipId);
            int start = timeline->getItemPosition(clipId);
            int end = start + timeline->getItemPlaytime(clipId);
            timeline->checkRefresh(start, end);
        }
        return res;
    };
    undo = [timeline, clipId, oldState]() {
        bool res = timeline->m_allClips[clipId]->setClipState(oldState);
        // in order to make the producer change effective, we need to unplant / replant the clip in int track
        int trackId = timeline->getClipTrackId(clipId);
        std::function<bool(void)> local_undo = []() { return true; };
        std::function<bool(void)> local_redo = []() { return true; };
        if (trackId != -1) {
            int start = timeline->getItemPosition(clipId);
            int end = start + timeline->getItemPlaytime(clipId);
            timeline->getTrackById(trackId)->replugClip(clipId);
            QModelIndex ix = timeline->makeClipIndexFromID(clipId);
            timeline->dataChanged(ix, ix, {TimelineModel::StatusRole});
            timeline->invalidateClip(clipId);
            timeline->checkRefresh(start, end);
        }
        return res;
    };
    bool result = redo();
    if (result) {
        pCore->pushUndo(undo, redo, i18n("Change clip state"));
    }
    return result;
}

bool TimelineFunctions::requestSplitAudio(std::shared_ptr<TimelineItemModel> timeline, int clipId)
{
    std::function<bool(void)> undo = []() { return true; };
    std::function<bool(void)> redo = []() { return true; };
    const std::unordered_set<int> clips = timeline->getGroupElements(clipId);
    int count = 0;
    for (int cid : clips) {
        int position = timeline->getClipPosition(cid);
        int duration = timeline->getClipPlaytime(cid);
        int track = timeline->getClipTrackId(cid);
        int newTrack = timeline->getNextTrackId(track);
        int newId;
        TimelineFunctions::changeClipState(timeline, clipId, PlaylistState::VideoOnly);
        bool res = copyClip(timeline, cid, newId, PlaylistState::AudioOnly, undo, redo);
        res = res && timeline->requestClipMove(newId, newTrack, position, true, false, undo, redo);
        std::unordered_set<int> clips;
        clips.insert(clipId);
        clips.insert(newId);
        timeline->requestClipsGroup(clips, true);
        if (!res) {
            bool undone = undo();
            Q_ASSERT(undone);
            return false;
        }
    }
    pCore->pushUndo(undo, redo, i18n("Split Audio"));
    return true;
}