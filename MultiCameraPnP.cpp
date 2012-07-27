/*
 *  MultiCameraPnP.cpp
 *  SfMToyExample
 *
 *  Created by Roy Shilkrot on 4/27/12.
 *  Copyright 2012 MIT. All rights reserved.
 *
 */

#include "MultiCameraPnP.h"
#include "BundleAdjuster.h"

using namespace std;

#include <opencv2/gpu/gpu.hpp>

/**
 * Get an initial 3D point cloud from 2 views only
 */
void MultiCameraPnP::GetBaseLineTriangulation() {
	std::cout << "=========================== Baseline triangulation ===========================\n";

	cv::Matx34d P(1,0,0,0,
				0,1,0,0,
				0,0,1,0);

	std::vector<CloudPoint> tmp_pcloud;
	
	//Reconstruct from two views
	bool goodF = false;
	second_view = 0; //start with 0th and 1st views
	while (!goodF && second_view < imgpts.size()) {
		//what if reconstrcution of first two views is bad? fallback to another pair
		second_view++; //go to the next view...
		//See if the Fundamental Matrix between these two views is good
		goodF = FindCameraMatrices(K, Kinv, 
								   imgpts[0], 
								   imgpts[second_view], 
								   imgpts_good[0],
								   imgpts_good[second_view], 
								   P, 
								   Pmats[std::make_pair(0,second_view)],
								   matches_matrix[std::make_pair(0,second_view)],
								   tmp_pcloud
#ifdef __SFM__DEBUG__
								   ,imgs[0],imgs[second_view]
#endif
								   );

	}
	
	//TODO: if the P1 matrix is far away from identity rotation - the solution is probably invalid...
	//so use an identity matrix?
	
	
	double reproj_error;
	{
		std::vector<cv::KeyPoint> pt_set1,pt_set2;
		
		std::vector<cv::DMatch> matches = matches_matrix[std::make_pair(0,second_view)];

		GetAlignedPointsFromMatch(imgpts[0],imgpts[second_view],matches,pt_set1,pt_set2);
		
		reproj_error = TriangulatePoints(pt_set1, 
										 pt_set2, 
										 Kinv, 
										 P, 
										 Pmats[std::make_pair(0,second_view)], 
										 pcloud, 
										 correspImg1Pt);
		
		std::cout << "pt_set1.size() " << pt_set1.size() << 
					" pt_set2.size() " << pt_set2.size() << 
					" matches.size() " << matches.size() << 
					" pcloud.size() " << pcloud.size() <<
					std::endl;
		
		for (unsigned int i=0; i<pcloud.size(); i++) {
			pcloud[i].imgpt_for_img = std::vector<int>(imgs.size(),-1);
			//matches[i] corresponds to pointcloud[i]
			pcloud[i].imgpt_for_img[0] = matches[i].queryIdx;
			pcloud[i].imgpt_for_img[second_view] = matches[i].trainIdx;
		}
	}
	std::cout << "triangulation reproj error " << reproj_error << std::endl;
}

void MultiCameraPnP::RecoverDepthFromImages() {
	if(!features_matched) 
		OnlyMatchFeatures();
	
	std::cout << "======================================================================\n";
	std::cout << "======================== Depth Recovery Start ========================\n";
	std::cout << "======================================================================\n";
	
	cv::Matx34d P(1,0,0,0,
				  0,1,0,0,
				  0,0,1,0);
	Pmats[std::make_pair(0,0)] = P;

	GetBaseLineTriangulation();
	
	cv::Matx34d P1 = Pmats[std::make_pair(0,second_view)];
	
	cv::Mat_<double> distcoeff(1,4,0.0);
	cv::Mat K_32f; K.convertTo(K_32f,CV_32FC1);
	cv::Mat distcoeff_32f; distcoeff.convertTo(distcoeff_32f,CV_32FC1);

	cv::Mat_<double> t = (cv::Mat_<double>(1,3) << P1(0,3), P1(1,3), P1(2,3));
	cv::Mat_<double> R = (cv::Mat_<double>(3,3) << P1(0,0), P1(0,1), P1(0,2), 
						  P1(1,0), P1(1,1), P1(1,2), 
						  P1(2,0), P1(2,1), P1(2,2));
	cv::Mat_<double> rvec(1,3); Rodrigues(R, rvec);
	
	//loop images to incrementally recover more cameras 
	for (unsigned int i=1; i < imgs.size(); i++) {
		if(i==second_view) continue; //baseline is already in cloud...

		// start scanning previous views
		int view = i-1;
		for (; view >= 0; view--) 
		{
			std::cout << "-------------------------- " << imgs_names[view] << " -> " << imgs_names[i] << " --------------------------\n";

			std::vector<cv::Point3f> ppcloud;
			std::vector<cv::Point2f> imgPoints;
			
			//prune the match between i and <view> using the Fundamental matrix to prune
			GetFundamentalMat( imgpts[view], 
							  imgpts[i], 
							  imgpts_good[view],
							  imgpts_good[i], 
							  matches_matrix[std::make_pair(view,i)]
							  );
			
			{
				//check for matches between i'th frame and <view>'th frame (and thus the current cloud)
				std::vector<cv::DMatch> matches = matches_matrix[std::make_pair(view,i)];
				for (unsigned int pt_img_view=0; pt_img_view<matches.size(); pt_img_view++) {
					// the index of the matching point in <view>
					int matches_img0_queryIdx = matches[pt_img_view].queryIdx;
				
					//scan the existing cloud (pcloud) to see if this point from <view> exists
					for (unsigned int pcldp=0; pcldp<pcloud.size(); pcldp++) {
						// see if corresponding point was found in cloud
						if (matches_img0_queryIdx == pcloud[pcldp].imgpt_for_img[view]) {
							//3d point in cloud
							ppcloud.push_back(pcloud[pcldp].pt);
							//2d point in image i
							imgPoints.push_back(imgpts[i][matches[pt_img_view].trainIdx].pt);
						
							break;
						}
					}
				}
			}

			if(ppcloud.size() <= 7 || imgPoints.size() <= 7 || ppcloud.size() != imgPoints.size()) { 
				//something went wrong aligning 3D to 2D points..
				cerr << "couldn't find [enough] corresponding cloud points... (only " << ppcloud.size() << ")" <<endl;
				continue;
			}
		
			if(!use_gpu)
				cv::solvePnPRansac(ppcloud, imgPoints, K, distcoeff, rvec, t, true);
//				cv::solvePnP(ppcloud, imgPoints, K, distcoeff, rvec, t, false, CV_EPNP);
			else {
				//use GPU ransac
				//make sure datatstructures are cv::gpu compatible
				cv::Mat ppcloud_m(ppcloud); ppcloud_m = ppcloud_m.t();
				cv::Mat imgPoints_m(imgPoints); imgPoints_m = imgPoints_m.t();
				cv::Mat rvec_,t_;

				cv::gpu::solvePnPRansac(ppcloud_m,imgPoints_m,K_32f,distcoeff_32f,rvec_,t_,false);
			
				rvec_.convertTo(rvec,CV_64FC1);
				t_.convertTo(t,CV_64FC1);
			}
		
			if(cv::norm(t) > 200.0) {
				// this is bad...
				cerr << "estimated camera movement is too big, skip this camera\r\n";
				continue;
			}

			Rodrigues(rvec, R);
			if(!CheckCoherentRotation(R)) {
				cerr << "rotation is incoherent. we should try a different base view..." << endl;
				continue;
			}
		
			std::cout << "found t = " << t << "\nR = \n"<<R<<std::endl;
		
			P1 = cv::Matx34d(R(0,0),R(0,1),R(0,2),t(0),
							 R(1,0),R(1,1),R(1,2),t(1),
							 R(2,0),R(2,1),R(2,2),t(2));
		
			//since we are using PnP , all P cam-pose matrices are aligned to the origin 0,0,0 camera...
			Pmats[std::make_pair(0,i)] = P1;
		
			//get the left camera matrix
			//TODO: potential bug - the P mat for <view> may not exist? or does it...
			P = Pmats[std::make_pair(0,view)];
		
			std::vector<cv::KeyPoint> pt_set1,pt_set2;
			std::vector<cv::DMatch> matches = matches_matrix[std::make_pair(view,i)];
			GetAlignedPointsFromMatch(imgpts[view],imgpts[i],matches,pt_set1,pt_set2);
		
			unsigned int start_i = pcloud.size();
		
			vector<CloudPoint> new_triangulated;
			
			//adding more triangulated points to general cloud
			double reproj_error = TriangulatePoints(pt_set1, pt_set2, Kinv, P, P1, new_triangulated, correspImg1Pt);
			std::cout << "triangulation reproj error " << reproj_error << std::endl;

			if(reproj_error > 100.0) {
				// somethign went awry, delete those triangulated points
//				pcloud.resize(start_i);
				cerr << "reprojection error too high, don't include these points."<<endl;
				continue;
			}

			vector<int> add_to_cloud(new_triangulated.size());
			int found_other_views_count = 0;
			for (unsigned int j = 0; j<matches.size(); j++) {
				new_triangulated[j].imgpt_for_img = std::vector<int>(imgs.size(),-1);

				//matches[j] corresponds to pointcloud[j]
				//matches[j].queryIdx = point in <view>
				//matches[j].trainIdx = point in <i>
				new_triangulated[j].imgpt_for_img[view] = matches[j].queryIdx; //2D reference to <view>
				bool found_in_other_view = false;
				for (unsigned int view_ = 1; view_ < i; view_++) {
					if(view_ == view) continue;
					std::vector<cv::DMatch> submatches = matches_matrix[std::make_pair(view_,i)];
					for (unsigned int ii = 0; ii < submatches.size(); ii++) {
						if (submatches[ii].trainIdx == matches[j].trainIdx) 
						{
							cout << "2d pt " << submatches[ii].queryIdx << " in img " << view_ << " matched 2d pt " << submatches[ii].trainIdx << " in img " << i << endl;
							//Point was alredy found in another view - strengthen it in the known cloud
							for (unsigned int pt3d=0; pt3d<pcloud.size(); pt3d++) {
								if (pcloud[pt3d].imgpt_for_img[view_] == submatches[ii].queryIdx) 
								{
									cout << "3d point "<<pt3d<<" in cloud, referenced 2d pt " << submatches[ii].queryIdx << " in view " << view_ << endl;
									pcloud[pt3d].imgpt_for_img[i] = matches[j].trainIdx;
									pcloud[pt3d].imgpt_for_img[view] = matches[j].queryIdx;
								}
							}
							
							found_in_other_view = true;
							add_to_cloud[j] = 0;
							break;
						}
					}
				}
				if (found_in_other_view) {
					found_other_views_count++;
				} else {
					add_to_cloud[j] = 1;
				}

				new_triangulated[j].imgpt_for_img[i] = matches[j].trainIdx;
			}
			std::cout << found_other_views_count << "/" << matches.size() << " points were found in other views\n";

			for (int j=0; j<add_to_cloud.size(); j++) {
				if(add_to_cloud[j] == 1)
					pcloud.push_back(new_triangulated[j]);
			}
			
			std::cout << "before triangulation: " << start_i << " after " << pcloud.size() << std::endl;

			break;
		}
	}

	for (unsigned int i=0; i<pcloud.size(); i++) {
		pointcloud_beforeBA.push_back(pcloud[i]);
		int good_view = 0;
		for(; good_view < imgs.size(); good_view++) {
			if(pcloud[i].imgpt_for_img[good_view] != -1) {
				int pt_idx = pcloud[i].imgpt_for_img[good_view];
				if(pt_idx >= imgpts[good_view].size()) {
					cerr << "BUG: point id:" << pt_idx << " should not exist for img #" << good_view << " which has only " << imgpts[good_view].size() << endl;
					continue;
				}
				pointCloudRGB_beforeBA.push_back(imgs_orig[good_view].at<cv::Vec3b>(imgpts[good_view][pt_idx].pt));
				break;
			}
		}
		if(good_view == imgs.size()) //nothing found.. put red dot
			pointCloudRGB_beforeBA.push_back(cv::Vec3b(255,0,0));
	}
	
	cout << "======================== Bundle Adjustment ==========================\n";
	
	BundleAdjuster BA;
	BA.adjustBundle(pcloud,cam_matrix,imgpts,Pmats);
	
	for (unsigned int i=0; i<pcloud.size(); i++) {
		pointcloud.push_back(pcloud[i]);
		int good_view = 0;
		for(; good_view < imgs.size(); good_view++) {
			if(pcloud[i].imgpt_for_img[good_view] != -1) {
				int pt_idx = pcloud[i].imgpt_for_img[good_view];
				if(pt_idx >= imgpts[good_view].size()) continue;
				pointCloudRGB.push_back(imgs_orig[good_view].at<cv::Vec3b>(imgpts[good_view][pt_idx].pt));
				break;
			}
		}
		if(good_view == imgs.size()) //nothing found.. put red dot
			pointCloudRGB.push_back(cv::Vec3b(255,0,0));
	}
	
	cout << "======================================================================\n";
	cout << "========================= Depth Recovery DONE ========================\n";
	cout << "======================================================================\n";
}
