#include "KinServer.h"

using namespace cv;

void KinServer::_onBlobCamServer(const Mat1w& depth, const Mat3b& depthClr, const Mat1b& playerIdx)
{
    threshed_depth.setTo(CV_BLACK);
    float x0 = corners[CORNER_DEPTH_LT].x - depthOrigin.x;
    float x1 = corners[CORNER_DEPTH_RB].x - depthOrigin.x;
    float y0 = corners[CORNER_DEPTH_LT].y - depthOrigin.y;
    float y1 = corners[CORNER_DEPTH_RB].y - depthOrigin.y;
    for (int y=y0;y<y1;y++)
    {
        for (int x=x0;x<x1;x++)
        {
            const Vec3b& clr = depthClr(y,x);
            uchar& out = threshed_depth(y,x);
            if ((clr[0] != clr[1]) || (clr[1] != clr[2]))
                out = 255;
        }
    }
    LOCK_START(mtx_depth_data_view);
    threshed_depth.copyTo(blob_view);
    if (open_param > 0)
        vOpen(threshed_depth, open_param);
    vFindBlobs(threshed_depth, native_blobs, min_area, DEPTH_WIDTH*DEPTH_HEIGHT);
    z_of_blobs.clear();
    id_of_blobs.clear();
    for (int i=0;i<native_blobs.size();i++)
    {
        // 			if (native_blobs[i].isHole)
        // 				continue;
        Point2f ct = native_blobs[i].center;
        ushort z = depth(ct.y, ct.x);
        z_of_blobs.push_back(z);
        int idx =  playerIdx(ct.y, ct.x) - 1;//TODO: make sure about the -1
        id_of_blobs.push_back(idx);

        circle(blob_view, ct, 10, CV_GRAY, 4);
    }
    LOCK_END();

    _sendBlobOsc();
}

void KinServer::sendTuioMessage(ofxOscSender& sender, const vBlobTracker& blobTracker)
{
    static int frameseq = 0;

    ofxOscBundle bundle;

    ofxOscMessage alive;
    {
        alive.setAddress("/tuio/2Dcur");
        alive.addStringArg("alive");
    }

    ofxOscMessage fseq;
    {
        fseq.setAddress( "/tuio/2Dcur" );
        fseq.addStringArg( "fseq" );
        fseq.addIntArg(frameseq++);
    }

    for (int i=0;i<blobTracker.trackedBlobs.size();i++)
    {	
        const vTrackedBlob& blob = blobTracker.trackedBlobs[i];

        Point2f center(blob.center.x + depthOrigin.x, blob.center.y + depthOrigin.y);

        // TODO: roi skip outside joints
        if (center.x <= corners[CORNER_DEPTH_LT].x
            || center.x >= corners[CORNER_DEPTH_RB].x
            || center.y <= corners[CORNER_DEPTH_LT].y
            || center.y >= corners[CORNER_DEPTH_RB].y
            )
            continue;

        ofxOscMessage set;
        set.setAddress( "/tuio/2Dcur" );
        set.addStringArg("set");
        set.addIntArg(blob.id);				// id
        set.addFloatArg((center.x - corners[CORNER_OUT_LT].x) / (corners[CORNER_OUT_RB].x - corners[CORNER_OUT_LT].x));
        set.addFloatArg((center.y - corners[CORNER_OUT_LT].y) / (corners[CORNER_OUT_RB].y - corners[CORNER_OUT_LT].y));
        set.addFloatArg(blob.velocity.x / DEPTH_WIDTH);
        set.addFloatArg(blob.velocity.y / DEPTH_HEIGHT);
        set.addFloatArg(0);		// m
        bundle.addMessage( set );							// add message to bundle

        alive.addIntArg(blob.id);				// add blob to list of ALL active IDs
    }

#undef addFloatX
#undef addFloatY

    bundle.addMessage( alive );	 //add message to bundle
    bundle.addMessage( fseq );	 //add message to bundle

    sender.sendBundle( bundle ); //send bundle
}

void KinServer::_onBlobCCV(const Mat1w& depth, const Mat3b& depthClr, const Mat1b& playerIdx)
{
    threshed_depth.setTo(CV_BLACK);
    float x0 = corners[CORNER_DEPTH_LT].x - depthOrigin.x;
    float x1 = corners[CORNER_DEPTH_RB].x - depthOrigin.x;
    float y0 = corners[CORNER_DEPTH_LT].y - depthOrigin.y;
    float y1 = corners[CORNER_DEPTH_RB].y - depthOrigin.y;

    for (int y=y0;y<y1;y++)
    {
        for (int x=x0;x<x1;x++)
        {
            ushort dep = depth(y,x);
            ushort bg = depth_bg(y,x);
            uchar& out = threshed_depth(y,x);
            if (dep > 0 && bg - dep > z_threshold_mm)
            { 
                out = 255; 
            }
        }
    }

    if (open_param > 0)
    {
        Mat element = getStructuringElement(MORPH_RECT, Size(open_param*2+1, open_param*2+1), Point(open_param, open_param) );
        morphologyEx(threshed_depth, threshed_depth, MORPH_OPEN, element);
    }

    LOCK_START(mtx_depth_data_view);
    threshed_depth.copyTo(blob_view);
    vFindBlobs(threshed_depth, native_blobs, min_area, DEPTH_WIDTH*DEPTH_HEIGHT);
    LOCK_END();

    blobTracker.trackBlobs(native_blobs);

    sendTuioMessage(*sender_tuio, blobTracker);
}

void KinServer::_sendBlobOsc(int/* = 0*/) 
{
    if (native_blobs.empty())
        return;

    {
        ofxOscMessage m;
        m.setAddress("/start");
        m.addIntArg(m_DeviceId);
        sender_osc->sendMessage(m);
    }

#define addFloatX(num) m.addFloatArg(num/(float)DEPTH_WIDTH)
#define addFloatY(num) m.addFloatArg(num/(float)DEPTH_HEIGHT)

    int n_blobs = native_blobs.size();
    for (int i = 0; i < n_blobs; i++)
    {
        float cz = z_of_blobs[i];
        if (cz < DEPTH_NEAR || cz > DEPTH_FAR)//skip illegal data
            continue;

        ofxOscMessage m;
        const vBlob& obj = native_blobs[i];
        int id = m_DeviceId*NUI_SKELETON_COUNT + id_of_blobs[i];
        float cx = obj.center.x;
        float cy = obj.center.y;		
        float w = obj.box.width;
        float h = obj.box.height;
        int nPts = obj.pts.size();

        if (obj.isHole)
            m.setAddress("/hole");
        else
            m.setAddress("/contour");

        m.addIntArg(id);                        //0
        m.addStringArg("/move");				//1
        addFloatX(cx);                          //2 -> cx
        addFloatY(cy);                          //3 -> cy
        addFloatX(w);							//4
        addFloatY(h);							//5 
        m.addFloatArg(cz);						//6 -> cz
        m.addIntArg(nPts);                      //7
        for (int k=0;k<nPts;k++)
        {
            addFloatX(obj.pts[k].x);
            addFloatY(obj.pts[k].y);
        }
        sender_osc->sendMessage( m );
    }
#undef addFloatX
#undef addFloatY
}
