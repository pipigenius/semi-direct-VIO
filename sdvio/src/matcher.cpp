#include <matcher.h>
#include <frame.h>
#include <feature.h>
#include <point.h>

namespace sdvio 
{

namespace warp
{
    
void getWarpMatrixAffine(
    camodocal::CameraPtr cam_ref,
    camodocal::CameraPtr cam_cur,
    const Vector2d& px_ref,
    const Vector3d& f_ref,
    const double depth_ref,   
    const SE3& T_cur_ref,
    const int level_ref,
    Matrix2d& A_cur_ref) 
{
    const int halfpatch_size = 5;
    const Vector3d xyz_ref(f_ref * depth_ref);
    Vector3d xyz_du_ref;
    Vector3d xyz_dv_ref;
    cam_ref->liftProjective(px_ref + Vector2d(halfpatch_size,0)*(1<<level_ref), xyz_du_ref);
    cam_ref->liftProjective(px_ref + Vector2d(0,halfpatch_size)*(1<<level_ref), xyz_dv_ref);
    xyz_du_ref.normalized();
    xyz_dv_ref.normalized();
    xyz_du_ref *= xyz_ref[2] / xyz_du_ref[2];
    xyz_dv_ref *= xyz_ref[2] / xyz_dv_ref[2];
    Vector2d px_cur;
    Vector2d px_du;
    Vector2d px_dv;
    cam_cur->spaceToPlane(T_cur_ref * xyz_ref, px_cur);
    cam_cur->spaceToPlane(T_cur_ref * xyz_du_ref, px_du);
    cam_cur->spaceToPlane(T_cur_ref * xyz_dv_ref, px_dv);
    A_cur_ref.col(0) = (px_du - px_cur) / halfpatch_size;
    A_cur_ref.col(1) = (px_dv - px_cur) / halfpatch_size;    
}

int getBestSearchLevel(
    const Matrix2d& A_cur_ref,
    const int max_level)
{
    int search_level = 0;
    double D = A_cur_ref.determinant();
    while (D > 3.0 && search_level < max_level) {
	search_level += 1;
	D *= 0.25;
    }
    return search_level;
}

void warpAffine(
    const Matrix2d& A_cur_ref,
    const cv::Mat& img_ref,
    const Vector2d& px_ref,
    const int level_ref,
    const int search_level,
    const int halfpatch_size,
    uint8_t* patch)
{
    const int patch_size = halfpatch_size * 2;
    const Matrix2f A_ref_cur = A_cur_ref.inverse().cast<float>();
    if (isnan(A_ref_cur(0.0))) {
	printf("Affine warp is NaN, probably camera has no translation\n"); 
	return;	
    }
    
    uint8_t* patch_ptr = patch;
    const Vector2f px_ref_pyr = px_ref.cast<float>() / (1<<level_ref);
    for (int y = 0; y < patch_size; ++y) {
	for (int x = 0; x < patch_size; ++x, ++patch_ptr) {
	    Vector2f px_patch(x - halfpatch_size, y - halfpatch_size);
	    px_patch *= (1<<search_level);
	    const Vector2f px(A_ref_cur * px_patch + px_ref_pyr);
	    if (px[0]<0 || px[1]<0 || px[0]>=img_ref.cols-1 || px[1]>=img_ref.rows-1)
		*patch_ptr = 0;
	    else
		*patch_ptr = (uint8_t) utility::interpolateMat_8u(img_ref, px[0], px[1]);
	}
    }
}  
    
}// end warp


bool depthFromTriangulation (
    const SE3& T_search_ref,
    const Vector3d& f_ref,
    const Vector3d& f_cur,
    double& depth)
{
    Matrix<double, 3, 2> A;
    A << T_search_ref.rotation_matrix() * f_ref, f_cur;
    const Matrix2d AtA = A.transpose() * A;
    if (AtA.determinant() < 0.000001)
	return false;
    const Vector2d depth2 = -AtA.inverse() * A.transpose() * T_search_ref.translation();
    depth = fabs(depth2[0]);
    return true;
}

void Matcher::createPatchFromPatchWithBorder()
{
    uint8_t* ref_patch_ptr = patch_;
    for (int y = 1; y < patch_size_ + 1; ++y, ref_patch_ptr += patch_size_) {
	uint8_t* ref_patch_border_ptr = patch_with_border_ + y * (patch_size_+2) +1;
	for (int x = 0; x < patch_size_; ++x) {
	    ref_patch_ptr[x] = ref_patch_border_ptr[x];
	}
    }
}

bool Matcher::findEpipolarMatchDirect(
    const Frame& ref_frame, 
    const Frame& cur_frame, 
    const Feature& ref_ftr, 
    const double d_estimate, 
    const double d_min, 
    const double d_max, 
    double& depth)
{
    SE3 T_cur_ref = cur_frame.T_w_b.inverse() * ref_frame.T_w_b;
    int zmssd_best = PatchScore::threshold();
    Vector2d uv_best;
    
    // Compute start and end of epipolar line in old_kf for match search, on unit plane!
    Vector2d A = utility::project2d(T_cur_ref * (ref_ftr.f * d_min));
    Vector2d B = utility::project2d(T_cur_ref * (ref_ftr.f * d_max));
    epi_dir_ = A - B; // epipolar vector in image 2
    
    // Compute affine warp matrix
    warp::getWarpMatrixAffine(ref_frame.cam, cur_frame.cam, ref_ftr.px, ref_ftr.f,
				d_estimate, T_cur_ref, ref_ftr.level, A_cur_ref_);
    
    // feature pre-selection
    reject_ = false;
    if (ref_ftr.type == Feature::EDGELET && options_.epi_search_edgelet_filtering) {
	const Vector2d grad_cur = (A_cur_ref_ * ref_ftr.grad).normalized();
	
	// if angle between grad and epi_dir is so big that tends to be vertical, it is hard to solve.
	const double cosangle = fabs(grad_cur.dot(epi_dir_.normalized()));
	if (cosangle < options_.epi_search_edgelet_max_angle) {
	    reject_ = true;
	    return false;
	}
    }
    
    search_level_ = warp::getBestSearchLevel(A_cur_ref_, NUM_PYR_LEVELS - 1);
    
    // Find length of search range on epipolar line
    Vector2d px_A(cur_frame.f2c(A));
    Vector2d px_B(cur_frame.f2c(A));
    epi_length_ = (px_A - px_B).norm() / (1<<search_level_);
    
    // Warp reference patch at ref_level
    warp::warpAffine(A_cur_ref_, ref_frame.img_pyr[ref_ftr.level], ref_ftr.px,
                   ref_ftr.level, search_level_, halfpatch_size_+1, patch_with_border_);
    
    createPatchFromPatchWithBorder();
    
    // If epi_length is small enough, we prefer to align them directly
    if (epi_length_ < 2.0) {
	px_cur_ = (px_A + px_B) / 2.0;
	Vector2d px_scaled(px_cur_ / (1<<search_level_));
	bool res;
	if (options_.align_1d) {
	    res = feature_alignment::align1D(
		cur_frame.img_pyr[search_level_], (px_A-px_B).cast<float>().normalized(),
		patch_with_border_, patch_, options_.align_max_iter, px_scaled, h_inv_);
	} else {
	    res = feature_alignment::align2D(
		cur_frame.img_pyr[search_level_], patch_with_border_, patch_,
		options_.align_max_iter, px_scaled);
	}
	
	if (res) {
	    // px estimate in current image
	    px_cur_ = px_scaled * (1<<search_level_);
	    if (depthFromTriangulation(T_cur_ref, ref_ftr.f, cur_frame.c2f(px_cur_), depth)) {
		return true;
	    }
	}
	return false;
    }
    
    // ZMSSD matcher
    size_t n_steps = epi_length_ / 0.7;
    Vector2d step = epi_dir_/n_steps;
    
    if(n_steps > options_.max_epi_search_steps)
    {
	printf("WARNING: skip epipolar search: %zu evaluations, px_lenght=%f, d_min=%f, d_max=%f.\n",
	    n_steps, epi_length_, d_min, d_max);
	return false;
    }    
    
    // for matching, precompute sum and sum2 of warped reference patch
//     int pixel_sum = 0;
//     int pixel_sum_square = 0;
    PatchScore patch_score(patch_);
    
    // now we sample along the epipolar line
    Vector2d uv = B - step; // pt in Fc
    Vector2i last_checked_pxi(0,0);
    ++n_steps;    
  
    // Find the patch that has the smallest zmss
    for (size_t i = 0; i < n_steps; ++i, uv+=step) {
	Vector2d px(cur_frame.f2c(uv));
	Vector2i pxi(px[0]/(1<<search_level_)+0.5, px[1]/(1<<search_level_)+0.5);
	
	if(pxi == last_checked_pxi)
	    continue;
	last_checked_pxi = pxi;
	
	// check if the patch is full within the new frame
	if(!cur_frame.isInFrame(pxi, patch_size_, search_level_))
	    continue;

	// current new patch created by step
	uint8_t* cur_patch_ptr = cur_frame.img_pyr[search_level_].data + (pxi[1]-halfpatch_size_)*cur_frame.img_pyr[search_level_].cols + (pxi[0]-halfpatch_size_);
	
	int zmssd = patch_score.computeScore(cur_patch_ptr, cur_frame.img_pyr[search_level_].cols);
	
	if(zmssd < zmssd_best) {
	    zmssd_best = zmssd;
	    uv_best = uv;
	}				
    }
    
    if(zmssd_best < PatchScore::threshold())
    {
	if(options_.subpix_refinement)
	{
	px_cur_ = cur_frame.f2c(uv_best);
	Vector2d px_scaled(px_cur_/(1<<search_level_));
	bool res;
	if(options_.align_1d)
	    res = feature_alignment::align1D(
		cur_frame.img_pyr[search_level_], (px_A-px_B).cast<float>().normalized(),
		patch_with_border_, patch_, options_.align_max_iter, px_scaled, h_inv_);
	else
	    res = feature_alignment::align2D(
		cur_frame.img_pyr[search_level_], patch_with_border_, patch_,
		options_.align_max_iter, px_scaled);
	if(res)
	{
	    px_cur_ = px_scaled*(1<<search_level_);
	    if(depthFromTriangulation(T_cur_ref, ref_ftr.f, cur_frame.c2f(px_cur_), depth))
	    return true;
	}
	return false;
	}
	
	px_cur_ = cur_frame.f2c(uv_best);
	if(depthFromTriangulation(T_cur_ref, ref_ftr.f, utility::unproject2d(uv_best).normalized(), depth))
	return true;
    }
    return false;    
}

bool Matcher::findMatchDirect(const Point& pt, const Frame& cur_frame, Vector2d& px_cur)
{
    if(!pt.getCloseViewObs(cur_frame.pos(), ref_ftr_))
	return false;

    if(!ref_ftr_->frame->isInFrame(
	ref_ftr_->px.cast<int>()/(1<<ref_ftr_->level), halfpatch_size_+2, ref_ftr_->level))
	return false;
    
    // warp affine
    SE3 T_cur_ref = cur_frame.T_w_b.inverse() * ref_ftr_->frame->T_w_b;
    warp::getWarpMatrixAffine(
	ref_ftr_->frame->cam, cur_frame.cam, ref_ftr_->px, ref_ftr_->f,
	(ref_ftr_->frame->pos() - pt.pos).norm(),
	T_cur_ref, ref_ftr_->level, A_cur_ref_);
    search_level_ = warp::getBestSearchLevel(A_cur_ref_, NUM_PYR_LEVELS-1);
    warp::warpAffine(A_cur_ref_, ref_ftr_->frame->img_pyr[ref_ftr_->level], ref_ftr_->px,
		    ref_ftr_->level, search_level_, halfpatch_size_+1, patch_with_border_);
    createPatchFromPatchWithBorder();

    // px_cur should be set
    Vector2d px_scaled(px_cur/(1<<search_level_));

    bool success = false;
    if(ref_ftr_->type == Feature::EDGELET)
    {
	Vector2d dir_cur(A_cur_ref_*ref_ftr_->grad);
	dir_cur.normalize();
	success = feature_alignment::align1D(
	    cur_frame.img_pyr[search_level_], dir_cur.cast<float>(),
	    patch_with_border_, patch_, options_.align_max_iter, px_scaled, h_inv_);
    }
    else
    {
	success = feature_alignment::align2D(
	cur_frame.img_pyr[search_level_], patch_with_border_, patch_,
	options_.align_max_iter, px_scaled);
    }
    px_cur = px_scaled * (1<<search_level_);
    return success;    
}

}