/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2004-2017 Jean-Pierre Charras, jp.charras at wanadoo.fr
 * Copyright (C) 2014 Dick Hollenbeck, dick@softplc.com
 * Copyright (C) 2017 KiCad Developers, see change_log.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

/**
 * @file drc.cpp
 */

#include <fctsys.h>
#include <wxPcbStruct.h>
#include <trigo.h>
#include <base_units.h>
#include <class_board_design_settings.h>

#include <class_module.h>
#include <class_track.h>
#include <class_pad.h>
#include <class_zone.h>
#include <class_pcb_text.h>
#include <class_draw_panel_gal.h>
#include <view/view.h>
#include <geometry/seg.h>

#include <connectivity.h>
#include <connectivity_algo.h>

#include <tool/tool_manager.h>
#include <tools/pcb_actions.h>

#include <pcbnew.h>
#include <drc_stuff.h>

#include <dialog_drc.h>
#include <wx/progdlg.h>
#include <board_commit.h>

void DRC::ShowDRCDialog( wxWindow* aParent )
{
    bool show_dlg_modal = true;

    // the dialog needs a parent frame. if it is not specified, this is
    // the PCB editor frame specified in DRC class.
    if( aParent == NULL )
    {
        // if any parent is specified, the dialog is modal.
        // if this is the default PCB editor frame, it is not modal
        show_dlg_modal = false;
        aParent = m_pcbEditorFrame;
    }

    if( !m_drcDialog )
    {
        m_pcbEditorFrame->GetToolManager()->RunAction( PCB_ACTIONS::selectionClear, true );
        m_drcDialog = new DIALOG_DRC_CONTROL( this, m_pcbEditorFrame, aParent );
        updatePointers();

        m_drcDialog->SetRptSettings( m_doCreateRptFile, m_rptFilename);

        if( show_dlg_modal )
            m_drcDialog->ShowModal();
        else
            m_drcDialog->Show( true );
    }
    else    // The dialog is just not visible (because the user has double clicked on an error item)
    {
        updatePointers();
        m_drcDialog->Show( true );
    }
}

void DRC::addMarkerToPcb( MARKER_PCB* aMarker )
{
    BOARD_COMMIT commit ( m_pcbEditorFrame );
    commit.Add( aMarker );
    commit.Push( wxEmptyString, false );
}

void DRC::DestroyDRCDialog( int aReason )
{
    if( m_drcDialog )
    {
        if( aReason == wxID_OK )
        {
            // if user clicked OK, save his choices in this DRC object.
            m_drcDialog->GetRptSettings( &m_doCreateRptFile, m_rptFilename);
        }

        m_drcDialog->Destroy();
        m_drcDialog = NULL;
    }
}


DRC::DRC( PCB_EDIT_FRAME* aPcbWindow )
{
    m_pcbEditorFrame = aPcbWindow;
    m_pcb = aPcbWindow->GetBoard();
    m_drcDialog  = NULL;

    // establish initial values for everything:
    m_doPad2PadTest     = true;     // enable pad to pad clearance tests
    m_doUnconnectedTest = true;     // enable unconnected tests
    m_doZonesTest = true;           // enable zone to items clearance tests
    m_doKeepoutTest = true;         // enable keepout areas to items clearance tests
    m_doFootprintOverlapping = true; // enable courtyards areas overlap tests
    m_doNoCourtyardDefined = true;  // enable missing courtyard in footprint warning
    m_abortDRC = false;
    m_drcInProgress = false;

    m_doCreateRptFile = false;

    // m_rptFilename set to empty by its constructor

    m_currentMarker = NULL;

    m_segmAngle  = 0;
    m_segmLength = 0;

    m_xcliplo = 0;
    m_ycliplo = 0;
    m_xcliphi = 0;
    m_ycliphi = 0;
}


DRC::~DRC()
{
    // maybe someday look at pointainer.h  <- google for "pointainer.h"
    for( unsigned i = 0; i<m_unconnected.size();  ++i )
        delete m_unconnected[i];
}


int DRC::Drc( TRACK* aRefSegm, TRACK* aList )
{
    updatePointers();

    if( !doTrackDrc( aRefSegm, aList, true ) )
    {
        wxASSERT( m_currentMarker );

        m_pcbEditorFrame->SetMsgPanel( m_currentMarker );
        return BAD_DRC;
    }

    if( !doTrackKeepoutDrc( aRefSegm ) )
    {
        wxASSERT( m_currentMarker );

        m_pcbEditorFrame->SetMsgPanel( m_currentMarker );
        return BAD_DRC;
    }

    return OK_DRC;
}


int DRC::Drc( ZONE_CONTAINER* aArea, int aCornerIndex )
{
    updatePointers();

    if( !doEdgeZoneDrc( aArea, aCornerIndex ) )
    {
        wxASSERT( m_currentMarker );
        m_pcbEditorFrame->SetMsgPanel( m_currentMarker );
        return BAD_DRC;
    }

    return OK_DRC;
}


void DRC::RunTests( wxTextCtrl* aMessages )
{
    // be sure m_pcb is the current board, not a old one
    // ( the board can be reloaded )
    m_pcb = m_pcbEditorFrame->GetBoard();

    // someone should have cleared the two lists before calling this.

    if( !testNetClasses() )
    {
        // testing the netclasses is a special case because if the netclasses
        // do not pass the BOARD_DESIGN_SETTINGS checks, then every member of a net
        // class (a NET) will cause its items such as tracks, vias, and pads
        // to also fail.  So quit after *all* netclass errors have been reported.
        if( aMessages )
            aMessages->AppendText( _( "Aborting\n" ) );

        // update the m_drcDialog listboxes
        updatePointers();

        return;
    }

    // test pad to pad clearances, nothing to do with tracks, vias or zones.
    if( m_doPad2PadTest )
    {
        if( aMessages )
        {
            aMessages->AppendText( _( "Pad clearances...\n" ) );
            wxSafeYield();
        }

        testPad2Pad();
    }

    // test track and via clearances to other tracks, pads, and vias
    if( aMessages )
    {
        aMessages->AppendText( _( "Track clearances...\n" ) );
        wxSafeYield();
    }

    testTracks( aMessages ? aMessages->GetParent() : m_pcbEditorFrame, true );

    // Before testing segments and unconnected, refill all zones:
    // this is a good caution, because filled areas can be outdated.
    if( aMessages )
    {
        aMessages->AppendText( _( "Fill zones...\n" ) );
        wxSafeYield();
    }

    m_pcbEditorFrame->Fill_All_Zones( aMessages ? aMessages->GetParent() : m_pcbEditorFrame,
                                  false );

    // test zone clearances to other zones
    if( aMessages )
    {
        aMessages->AppendText( _( "Test zones...\n" ) );
        wxSafeYield();
    }

    testZones();

    // find and gather unconnected pads.
    if( m_doUnconnectedTest )
    {
        if( aMessages )
        {
            aMessages->AppendText( _( "Unconnected pads...\n" ) );
            aMessages->Refresh();
        }

        testUnconnected();
    }

    // find and gather vias, tracks, pads inside keepout areas.
    if( m_doKeepoutTest )
    {
        if( aMessages )
        {
            aMessages->AppendText( _( "Keepout areas ...\n" ) );
            aMessages->Refresh();
        }

        testKeepoutAreas();
    }

    // find and gather vias, tracks, pads inside text boxes.
    if( aMessages )
    {
        aMessages->AppendText( _( "Test texts...\n" ) );
        wxSafeYield();
    }

    testTexts();

    // find overlaping courtyard ares.
    if( m_doFootprintOverlapping || m_doNoCourtyardDefined )
    {
        if( aMessages )
        {
            aMessages->AppendText( _( "Courtyard areas...\n" ) );
            aMessages->Refresh();
        }

        doFootprintOverlappingDrc();
    }

    // update the m_drcDialog listboxes
    updatePointers();

    if( aMessages )
    {
        // no newline on this one because it is last, don't want the window
        // to unnecessarily scroll.
        aMessages->AppendText( _( "Finished" ) );
    }
}


void DRC::ListUnconnectedPads()
{
    testUnconnected();

    // update the m_drcDialog listboxes
    updatePointers();
}


void DRC::updatePointers()
{
    // update my pointers, m_pcbEditorFrame is the only unchangeable one
    m_pcb = m_pcbEditorFrame->GetBoard();

    if( m_drcDialog )  // Use diag list boxes only in DRC dialog
    {
        m_drcDialog->m_ClearanceListBox->SetList( new DRC_LIST_MARKERS( m_pcb ) );
        m_drcDialog->m_UnconnectedListBox->SetList( new DRC_LIST_UNCONNECTED( &m_unconnected ) );

        m_drcDialog->UpdateDisplayedCounts();
    }
}


bool DRC::doNetClass( NETCLASSPTR nc, wxString& msg )
{
    bool ret = true;

    const BOARD_DESIGN_SETTINGS& g = m_pcb->GetDesignSettings();

#define FmtVal( x ) GetChars( StringFromValue( g_UserUnit, x ) )

#if 0   // set to 1 when (if...) BOARD_DESIGN_SETTINGS has a m_MinClearance value
    if( nc->GetClearance() < g.m_MinClearance )
    {
        msg.Printf( _( "NETCLASS: '%s' has Clearance:%s which is less than global:%s" ),
                    GetChars( nc->GetName() ),
                    FmtVal( nc->GetClearance() ),
                    FmtVal( g.m_TrackClearance )
                    );

        addMarkerToPcb( fillMarker( DRCE_NETCLASS_CLEARANCE, msg, m_currentMarker ) );
        m_currentMarker = nullptr;
        ret = false;
    }
#endif

    if( nc->GetTrackWidth() < g.m_TrackMinWidth )
    {
        msg.Printf( _( "NETCLASS: '%s' has TrackWidth:%s which is less than global:%s" ),
                    GetChars( nc->GetName() ),
                    FmtVal( nc->GetTrackWidth() ),
                    FmtVal( g.m_TrackMinWidth )
                    );

        addMarkerToPcb( fillMarker( DRCE_NETCLASS_TRACKWIDTH, msg, m_currentMarker ) );
        m_currentMarker = nullptr;
        ret = false;
    }

    if( nc->GetViaDiameter() < g.m_ViasMinSize )
    {
        msg.Printf( _( "NETCLASS: '%s' has Via Dia:%s which is less than global:%s" ),
                    GetChars( nc->GetName() ),
                    FmtVal( nc->GetViaDiameter() ),
                    FmtVal( g.m_ViasMinSize )
                    );

        addMarkerToPcb( fillMarker( DRCE_NETCLASS_VIASIZE, msg, m_currentMarker ) );
        m_currentMarker = nullptr;
        ret = false;
    }

    if( nc->GetViaDrill() < g.m_ViasMinDrill )
    {
        msg.Printf( _( "NETCLASS: '%s' has Via Drill:%s which is less than global:%s" ),
                    GetChars( nc->GetName() ),
                    FmtVal( nc->GetViaDrill() ),
                    FmtVal( g.m_ViasMinDrill )
                    );

        addMarkerToPcb( fillMarker( DRCE_NETCLASS_VIADRILLSIZE, msg, m_currentMarker ) );
        m_currentMarker = nullptr;
        ret = false;
    }

    if( nc->GetuViaDiameter() < g.m_MicroViasMinSize )
    {
        msg.Printf( _( "NETCLASS: '%s' has uVia Dia:%s which is less than global:%s" ),
                    GetChars( nc->GetName() ),
                    FmtVal( nc->GetuViaDiameter() ),
                    FmtVal( g.m_MicroViasMinSize )
                    );

        addMarkerToPcb( fillMarker( DRCE_NETCLASS_uVIASIZE, msg, m_currentMarker ) );
        m_currentMarker = nullptr;
        ret = false;
    }

    if( nc->GetuViaDrill() < g.m_MicroViasMinDrill )
    {
        msg.Printf( _( "NETCLASS: '%s' has uVia Drill:%s which is less than global:%s" ),
                    GetChars( nc->GetName() ),
                    FmtVal( nc->GetuViaDrill() ),
                    FmtVal( g.m_MicroViasMinDrill )
                    );

        addMarkerToPcb( fillMarker( DRCE_NETCLASS_uVIADRILLSIZE, msg, m_currentMarker ) );
        m_currentMarker = nullptr;
        ret = false;
    }

    return ret;
}


bool DRC::testNetClasses()
{
    bool        ret = true;

    NETCLASSES& netclasses = m_pcb->GetDesignSettings().m_NetClasses;

    wxString    msg;   // construct this only once here, not in a loop, since somewhat expensive.

    if( !doNetClass( netclasses.GetDefault(), msg ) )
        ret = false;

    for( NETCLASSES::const_iterator i = netclasses.begin();  i != netclasses.end();  ++i )
    {
        NETCLASSPTR nc = i->second;

        if( !doNetClass( nc, msg ) )
            ret = false;
    }

    return ret;
}


void DRC::testPad2Pad()
{
    std::vector<D_PAD*> sortedPads;

    m_pcb->GetSortedPadListByXthenYCoord( sortedPads );

    // find the max size of the pads (used to stop the test)
    int max_size = 0;

    for( unsigned i = 0; i < sortedPads.size(); ++i )
    {
        D_PAD* pad = sortedPads[i];

        // GetBoundingRadius() is the radius of the minimum sized circle fully containing the pad
        int radius = pad->GetBoundingRadius();
        if( radius > max_size )
            max_size = radius;
    }

    // Test the pads
    D_PAD** listEnd = &sortedPads[ sortedPads.size() ];

    for( unsigned i = 0; i< sortedPads.size(); ++i )
    {
        D_PAD* pad = sortedPads[i];

        int    x_limit = max_size + pad->GetClearance() +
                         pad->GetBoundingRadius() + pad->GetPosition().x;

        if( !doPadToPadsDrc( pad, &sortedPads[i], listEnd, x_limit ) )
        {
            wxASSERT( m_currentMarker );
            addMarkerToPcb ( m_currentMarker );
            m_currentMarker = nullptr;
        }
    }
}


void DRC::testTracks( wxWindow *aActiveWindow, bool aShowProgressBar )
{
    wxProgressDialog * progressDialog = NULL;
    const int delta = 500;  // This is the number of tests between 2 calls to the
                            // progress bar
    int count = 0;
    for( TRACK* segm = m_pcb->m_Track; segm && segm->Next(); segm = segm->Next() )
        count++;

    int deltamax = count/delta;

    if( aShowProgressBar && deltamax > 3 )
    {
        progressDialog = new wxProgressDialog( _( "Track clearances" ), wxEmptyString,
                                               deltamax, aActiveWindow,
                                               wxPD_AUTO_HIDE | wxPD_CAN_ABORT |
                                               wxPD_APP_MODAL | wxPD_ELAPSED_TIME );
        progressDialog->Update( 0, wxEmptyString );
    }

    int ii = 0;
    count = 0;

    for( TRACK* segm = m_pcb->m_Track; segm; segm = segm->Next() )
    {
        if ( ii++ > delta )
        {
            ii = 0;
            count++;

            if( progressDialog )
            {
                if( !progressDialog->Update( count, wxEmptyString ) )
                    break;  // Aborted by user
#ifdef __WXMAC__
                // Work around a dialog z-order issue on OS X
                if( count == deltamax )
                    aActiveWindow->Raise();
#endif
            }
        }

        if( !doTrackDrc( segm, segm->Next(), true ) )
        {
            wxASSERT( m_currentMarker );
            addMarkerToPcb ( m_currentMarker );
            m_currentMarker = nullptr;
        }
    }

    if( progressDialog )
        progressDialog->Destroy();
}


void DRC::testUnconnected()
{

    auto connectivity = m_pcb->GetConnectivity();

    connectivity->Clear();
    connectivity->Build(m_pcb); // just in case. This really needs to be reliable.
    connectivity->RecalculateRatsnest();

    std::vector<CN_EDGE> edges;
    connectivity->GetUnconnectedEdges( edges );

    for( const auto& edge : edges )
    {
        wxString t_src = edge.GetSourceNode()->Parent()->GetSelectMenuText();
        wxString t_dst = edge.GetTargetNode()->Parent()->GetSelectMenuText();
        auto src = edge.GetSourcePos();
        auto dst = edge.GetTargetPos();


        DRC_ITEM* uncItem = new DRC_ITEM( DRCE_UNCONNECTED_ITEMS,
                                          t_src,
                                          t_dst,
                                          wxPoint( src.x, src.y ), wxPoint( dst.x, dst.y ) );
        m_unconnected.push_back( uncItem );

    }
}


void DRC::testZones()
{
    // Test copper areas for valid netcodes
    // if a netcode is < 0 the netname was not found when reading a netlist
    // if a netcode is == 0 the netname is void, and the zone is not connected.
    // This is allowed, but i am not sure this is a good idea
    //
    // In recent Pcbnew versions, the netcode is always >= 0, but an internal net name
    // is stored, and initalized from the file or the zone properpies editor.
    // if it differs from the net name from net code, there is a DRC issue
    for( int ii = 0; ii < m_pcb->GetAreaCount(); ii++ )
    {
        ZONE_CONTAINER* test_area = m_pcb->GetArea( ii );

        if( !test_area->IsOnCopperLayer() )
            continue;

        int netcode = test_area->GetNetCode();

        // a netcode < 0 or > 0 and no pad in net  is a error or strange
        // perhaps a "dead" net, which happens when all pads in this net were removed
        // Remark: a netcode < 0 should not happen (this is more a bug somewhere)
        int pads_in_net = (test_area->GetNetCode() > 0) ?
                            m_pcb->GetConnectivity()->GetPadCount( test_area->GetNetCode() ) : 1;

        if( ( netcode < 0 ) || pads_in_net == 0 )
        {
            addMarkerToPcb( fillMarker( test_area,
                                        DRCE_SUSPICIOUS_NET_FOR_ZONE_OUTLINE, m_currentMarker ) );
            m_currentMarker = nullptr;
        }
    }

    // Test copper areas outlines, and create markers when needed
    m_pcb->Test_Drc_Areas_Outlines_To_Areas_Outlines( NULL, true );
}


void DRC::testKeepoutAreas()
{
    // Test keepout areas for vias, tracks and pads inside keepout areas
    for( int ii = 0; ii < m_pcb->GetAreaCount(); ii++ )
    {
        ZONE_CONTAINER* area = m_pcb->GetArea( ii );

        if( !area->GetIsKeepout() )
        {
            continue;
        }

        for( TRACK* segm = m_pcb->m_Track; segm != NULL; segm = segm->Next() )
        {
            if( segm->Type() == PCB_TRACE_T )
            {
                if( ! area->GetDoNotAllowTracks()  )
                    continue;

                // Ignore if the keepout zone is not on the same layer
                if( !area->IsOnLayer( segm->GetLayer() ) )
                    continue;

                if( area->Outline()->Distance( SEG( segm->GetStart(), segm->GetEnd() ),
                                               segm->GetWidth() ) == 0 )
                {
                    addMarkerToPcb( fillMarker( segm, NULL,
                                                DRCE_TRACK_INSIDE_KEEPOUT, m_currentMarker ) );
                    m_currentMarker = nullptr;
                }
            }
            else if( segm->Type() == PCB_VIA_T )
            {
                if( ! area->GetDoNotAllowVias()  )
                    continue;

                auto viaLayers = segm->GetLayerSet();

                if( !area->CommonLayerExists( viaLayers ) )
                    continue;

                if( area->Outline()->Distance( segm->GetPosition() ) < segm->GetWidth()/2 )
                {
                    addMarkerToPcb( fillMarker( segm, NULL,
                                                DRCE_VIA_INSIDE_KEEPOUT, m_currentMarker ) );
                    m_currentMarker = nullptr;
                }
            }
        }
        // Test pads: TODO
    }
}


void DRC::testTexts()
{
    std::vector<wxPoint> textShape;      // a buffer to store the text shape (set of segments)
    std::vector<D_PAD*> padList = m_pcb->GetPads();

    // Test text areas for vias, tracks and pads inside text areas
    for( auto item : m_pcb->Drawings() )
    {
        // Drc test only items on copper layers
        if( ! IsCopperLayer( item->GetLayer() ) )
            continue;

        // only texts on copper layers are tested
        if( item->Type() !=  PCB_TEXT_T )
            continue;

        textShape.clear();

        // So far the bounding box makes up the text-area
        TEXTE_PCB* text = (TEXTE_PCB*) item;
        text->TransformTextShapeToSegmentList( textShape );

        if( textShape.size() == 0 )     // Should not happen (empty text?)
            continue;

        for( TRACK* track = m_pcb->m_Track; track != NULL; track = track->Next() )
        {
            if( ! track->IsOnLayer( item->GetLayer() ) )
                    continue;

            // Test the distance between each segment and the current track/via
            int min_dist = ( track->GetWidth() + text->GetThickness() ) /2 +
                           track->GetClearance(NULL);

            if( track->Type() == PCB_TRACE_T )
            {
                SEG segref( track->GetStart(), track->GetEnd() );

                // Error condition: Distance between text segment and track segment is
                // smaller than the clearance of the segment
                for( unsigned jj = 0; jj < textShape.size(); jj += 2 )
                {
                    SEG segtest( textShape[jj], textShape[jj+1] );
                    int dist = segref.Distance( segtest );

                    if( dist < min_dist )
                    {
                        addMarkerToPcb( fillMarker( track, text,
                                                    DRCE_TRACK_INSIDE_TEXT,
                                                    m_currentMarker ) );
                        m_currentMarker = nullptr;
                        break;
                    }
                }
            }
            else if( track->Type() == PCB_VIA_T )
            {
                // Error condition: Distance between text segment and via is
                // smaller than the clearance of the via
                for( unsigned jj = 0; jj < textShape.size(); jj += 2 )
                {
                    SEG segtest( textShape[jj], textShape[jj+1] );

                    if( segtest.PointCloserThan( track->GetPosition(), min_dist ) )
                    {
                        addMarkerToPcb( fillMarker( track, text,
                                                    DRCE_VIA_INSIDE_TEXT, m_currentMarker ) );
                        m_currentMarker = nullptr;
                        break;
                    }
                }
            }
        }

        // Test pads
        for( unsigned ii = 0; ii < padList.size(); ii++ )
        {
            D_PAD* pad = padList[ii];

            if( ! pad->IsOnLayer( item->GetLayer() ) )
                    continue;

            wxPoint shape_pos = pad->ShapePos();

            for( unsigned jj = 0; jj < textShape.size(); jj += 2 )
            {
                /* In order to make some calculations more easier or faster,
                 * pads and tracks coordinates will be made relative
                 * to the segment origin
                 */
                wxPoint origin = textShape[jj];  // origin will be the origin of other coordinates
                m_segmEnd = textShape[jj+1] - origin;
                wxPoint delta = m_segmEnd;
                m_segmAngle = 0;

                // for a non horizontal or vertical segment Compute the segment angle
                // in tenths of degrees and its length
                if( delta.x || delta.y )    // delta.x == delta.y == 0 for vias
                {
                    // Compute the segment angle in 0,1 degrees
                    m_segmAngle = ArcTangente( delta.y, delta.x );

                    // Compute the segment length: we build an equivalent rotated segment,
                    // this segment is horizontal, therefore dx = length
                    RotatePoint( &delta, m_segmAngle );    // delta.x = length, delta.y = 0
                }

                m_segmLength = delta.x;
                m_padToTestPos = shape_pos - origin;

                if( !checkClearanceSegmToPad( pad, text->GetThickness(),
                                              pad->GetClearance(NULL) ) )
                {
                    addMarkerToPcb( fillMarker( pad, text,
                                                DRCE_PAD_INSIDE_TEXT, m_currentMarker ) );
                    m_currentMarker = nullptr;
                    break;
                }
            }
        }
    }
}


bool DRC::doTrackKeepoutDrc( TRACK* aRefSeg )
{
    // Test keepout areas for vias, tracks and pads inside keepout areas
    for( int ii = 0; ii < m_pcb->GetAreaCount(); ii++ )
    {
        ZONE_CONTAINER* area = m_pcb->GetArea( ii );

        if( !area->GetIsKeepout() )
            continue;

        if( aRefSeg->Type() == PCB_TRACE_T )
        {
            if( ! area->GetDoNotAllowTracks()  )
                continue;

            if( !area->IsOnLayer( aRefSeg->GetLayer() ) )
                continue;

            if( area->Outline()->Distance( SEG( aRefSeg->GetStart(), aRefSeg->GetEnd() ),
                                           aRefSeg->GetWidth() ) == 0 )
            {
                m_currentMarker = fillMarker( aRefSeg, NULL,
                                              DRCE_TRACK_INSIDE_KEEPOUT, m_currentMarker );
                return false;
            }
        }
        else if( aRefSeg->Type() == PCB_VIA_T )
        {
            if( ! area->GetDoNotAllowVias()  )
                continue;

            auto viaLayers = aRefSeg->GetLayerSet();

            if( !area->CommonLayerExists( viaLayers ) )
                continue;

            if( area->Outline()->Distance( aRefSeg->GetPosition() ) < aRefSeg->GetWidth()/2 )
            {
                m_currentMarker = fillMarker( aRefSeg, NULL,
                                              DRCE_VIA_INSIDE_KEEPOUT, m_currentMarker );
                return false;
            }
        }
    }

    return true;
}


bool DRC::doPadToPadsDrc( D_PAD* aRefPad, D_PAD** aStart, D_PAD** aEnd, int x_limit )
{
    const static LSET all_cu = LSET::AllCuMask();

    LSET layerMask = aRefPad->GetLayerSet() & all_cu;

    /* used to test DRC pad to holes: this dummy pad has the size and shape of the hole
     * to test pad to pad hole DRC, using the pad to pad DRC test function.
     * Therefore, this dummy pad is a circle or an oval.
     * A pad must have a parent because some functions expect a non null parent
     * to find the parent board, and some other data
     */
    MODULE  dummymodule( m_pcb );    // Creates a dummy parent
    D_PAD   dummypad( &dummymodule );

    // Ensure the hole is on all copper layers
    dummypad.SetLayerSet( all_cu | dummypad.GetLayerSet() );

    // Use the minimal local clearance value for the dummy pad.
    // The clearance of the active pad will be used as minimum distance to a hole
    // (a value = 0 means use netclass value)
    dummypad.SetLocalClearance( 1 );

    for( D_PAD** pad_list = aStart;  pad_list<aEnd;  ++pad_list )
    {
        D_PAD* pad = *pad_list;

        if( pad == aRefPad )
            continue;

        // We can stop the test when pad->GetPosition().x > x_limit
        // because the list is sorted by X values
        if( pad->GetPosition().x > x_limit )
            break;

        // No problem if pads which are on copper layers are on different copper layers,
        // (pads can be only on a technical layer, to build complex pads)
        // but their hole (if any ) can create DRC error because they are on all
        // copper layers, so we test them
        if( ( pad->GetLayerSet() & layerMask ) == 0 &&
            ( pad->GetLayerSet() & all_cu ) != 0 &&
            ( aRefPad->GetLayerSet() & all_cu ) != 0 )
        {
            // if holes are in the same location and have the same size and shape,
            // this can be accepted
            if( pad->GetPosition() == aRefPad->GetPosition()
                && pad->GetDrillSize() == aRefPad->GetDrillSize()
                && pad->GetDrillShape() == aRefPad->GetDrillShape() )
            {
                if( aRefPad->GetDrillShape() == PAD_DRILL_SHAPE_CIRCLE )
                    continue;

                // for oval holes: must also have the same orientation
                if( pad->GetOrientation() == aRefPad->GetOrientation() )
                    continue;
            }

            /* Here, we must test clearance between holes and pads
             * dummy pad size and shape is adjusted to pad drill size and shape
             */
            if( pad->GetDrillSize().x )
            {
                // pad under testing has a hole, test this hole against pad reference
                dummypad.SetPosition( pad->GetPosition() );
                dummypad.SetSize( pad->GetDrillSize() );
                dummypad.SetShape( pad->GetDrillShape() == PAD_DRILL_SHAPE_OBLONG ?
                                                           PAD_SHAPE_OVAL : PAD_SHAPE_CIRCLE );
                dummypad.SetOrientation( pad->GetOrientation() );

                if( !checkClearancePadToPad( aRefPad, &dummypad ) )
                {
                    // here we have a drc error on pad!
                    m_currentMarker = fillMarker( pad, aRefPad,
                                                  DRCE_HOLE_NEAR_PAD, m_currentMarker );
                    return false;
                }
            }

            if( aRefPad->GetDrillSize().x ) // pad reference has a hole
            {
                dummypad.SetPosition( aRefPad->GetPosition() );
                dummypad.SetSize( aRefPad->GetDrillSize() );
                dummypad.SetShape( aRefPad->GetDrillShape() == PAD_DRILL_SHAPE_OBLONG ?
                                                               PAD_SHAPE_OVAL : PAD_SHAPE_CIRCLE );
                dummypad.SetOrientation( aRefPad->GetOrientation() );

                if( !checkClearancePadToPad( pad, &dummypad ) )
                {
                    // here we have a drc error on aRefPad!
                    m_currentMarker = fillMarker( aRefPad, pad,
                                                  DRCE_HOLE_NEAR_PAD, m_currentMarker );
                    return false;
                }
            }

            continue;
        }

        // The pad must be in a net (i.e pt_pad->GetNet() != 0 ),
        // But no problem if pads have the same netcode (same net)
        if( pad->GetNetCode() && ( aRefPad->GetNetCode() == pad->GetNetCode() ) )
            continue;

        // if pads are from the same footprint
        if( pad->GetParent() == aRefPad->GetParent() )
        {
            // and have the same pad number ( equivalent pads  )

            // one can argue that this 2nd test is not necessary, that any
            // two pads from a single module are acceptable.  This 2nd test
            // should eventually be a configuration option.
            if( pad->PadNameEqual( aRefPad ) )
                continue;
        }

        // if either pad has no drill and is only on technical layers, not a clearance violation
        if( ( ( pad->GetLayerSet() & layerMask ) == 0 && !pad->GetDrillSize().x ) ||
            ( ( aRefPad->GetLayerSet() & layerMask ) == 0 && !aRefPad->GetDrillSize().x ) )
        {
            continue;
        }

        if( !checkClearancePadToPad( aRefPad, pad ) )
        {
            // here we have a drc error!
            m_currentMarker = fillMarker( aRefPad, pad, DRCE_PAD_NEAR_PAD1, m_currentMarker );
            return false;
        }
    }

    return true;
}


bool DRC::doFootprintOverlappingDrc()
{
    // Detects missing (or malformed) footprint courtyard,
    // and for footprint with courtyard, courtyards overlap.
    wxString msg;
    bool success = true;

    // Update courtyard polygons, and test for missing courtyard definition:
    for( MODULE* footprint = m_pcb->m_Modules; footprint; footprint = footprint->Next() )
    {
        bool is_ok = footprint->BuildPolyCourtyard();

        if( !is_ok && m_doFootprintOverlapping )
        {
            msg.Printf( _( "footprint '%s' has malformed courtyard" ),
                        footprint->GetReference().GetData() );
            m_currentMarker = fillMarker( footprint->GetPosition(),
                                          DRCE_MALFORMED_COURTYARD_IN_FOOTPRINT,
                                          msg, m_currentMarker );
            addMarkerToPcb( m_currentMarker );
            m_currentMarker = nullptr;
            success = false;
        }

        if( !m_doNoCourtyardDefined )
            continue;

        if( footprint->GetPolyCourtyardFront().OutlineCount() == 0 &&
            footprint->GetPolyCourtyardBack().OutlineCount() == 0 &&
            is_ok )
        {
            msg.Printf( _( "footprint '%s' has no courtyard defined" ),
                        footprint->GetReference().GetData() );
            m_currentMarker = fillMarker( footprint->GetPosition(),
                                          DRCE_MISSING_COURTYARD_IN_FOOTPRINT,
                                          msg, m_currentMarker );
            addMarkerToPcb( m_currentMarker );
            m_currentMarker = nullptr;
            success = false;
        }
    }

    if( !m_doFootprintOverlapping )
        return success;

    // Now test for overlapping on top layer:
    SHAPE_POLY_SET courtyard;   // temporary storage of the courtyard of current footprint

    for( MODULE* footprint = m_pcb->m_Modules; footprint; footprint = footprint->Next() )
    {
        if( footprint->GetPolyCourtyardFront().OutlineCount() == 0 )
            continue;           // No courtyard defined

        for( MODULE* candidate = footprint->Next(); candidate; candidate = candidate->Next() )
        {
            if( candidate->GetPolyCourtyardFront().OutlineCount() == 0 )
                continue;       // No courtyard defined

            courtyard.RemoveAllContours();
            courtyard.Append( footprint->GetPolyCourtyardFront() );

            // Build the common area between footprint and the candidate:
            courtyard.BooleanIntersection( candidate->GetPolyCourtyardFront(), SHAPE_POLY_SET::PM_FAST );

            // If no overlap, courtyard is empty (no common area).
            // Therefore if a common polygon exists, this is a DRC error
            if( courtyard.OutlineCount() )
            {
                //Overlap between footprint and candidate
                msg.Printf( _( "footprints '%s' and '%s' overlap on front (top) layer" ),
                            footprint->GetReference().GetData(),
                            candidate->GetReference().GetData() );
                VECTOR2I& pos = courtyard.Vertex( 0, 0, -1 );
                wxPoint loc( pos.x, pos.y );
                m_currentMarker = fillMarker( loc, DRCE_OVERLAPPING_FOOTPRINTS, msg, m_currentMarker );
                addMarkerToPcb( m_currentMarker );
                m_currentMarker = nullptr;
                success = false;
            }
        }
    }

    // Test for overlapping on bottom layer:
    for( MODULE* footprint = m_pcb->m_Modules; footprint; footprint = footprint->Next() )
    {
        if( footprint->GetPolyCourtyardBack().OutlineCount() == 0 )
            continue;           // No courtyard defined

        for( MODULE* candidate = footprint->Next(); candidate; candidate = candidate->Next() )
        {
            if( candidate->GetPolyCourtyardBack().OutlineCount() == 0 )
                continue;       // No courtyard defined

            courtyard.RemoveAllContours();
            courtyard.Append( footprint->GetPolyCourtyardBack() );

            // Build the common area between footprint and the candidate:
            courtyard.BooleanIntersection( candidate->GetPolyCourtyardBack(), SHAPE_POLY_SET::PM_FAST );

            // If no overlap, courtyard is empty (no common area).
            // Therefore if a common polygon exists, this is a DRC error
            if( courtyard.OutlineCount() )
            {
                //Overlap between footprint and candidate
                msg.Printf( _( "footprints '%s' and '%s' overlap on back (bottom) layer" ),
                            footprint->GetReference().GetData(),
                            candidate->GetReference().GetData() );
                VECTOR2I& pos = courtyard.Vertex( 0, 0, -1 );
                wxPoint loc( pos.x, pos.y );
                m_currentMarker = fillMarker( loc, DRCE_OVERLAPPING_FOOTPRINTS, msg, m_currentMarker );
                addMarkerToPcb( m_currentMarker );
                m_currentMarker = nullptr;
                success = false;
            }
        }
    }

    return success;
}

