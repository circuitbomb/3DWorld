// 3D World - light source implementation
// by Frank Gennari
// 5/17/15
#include "mesh.h"
#include "lightmap.h"
#include "shaders.h"
#include "sinf.h"
#include "shadow_map.h"

extern int display_mode, camera_coll_id;
extern float fticks;
extern vector<light_source> light_sources_a;
extern vector<light_source_trig> light_sources_d;
extern coll_obj_group coll_objects;


bool bind_point_t::is_valid() { // used with placed dlights

	if (!bound) return 1; // if no binding point, always valid
	if (!valid) return 0; // already determined to be invalid

	if (bind_cobj < 0) { // cobj not yet found
		if (!check_point_contained_tree(bind_pos, bind_cobj, 0)) {valid = 0; return 0;}
		return 1;
	}
	assert((unsigned)bind_cobj < coll_objects.size());
	coll_obj const &cobj(coll_objects[bind_cobj]);
	if (cobj.status != COLL_STATIC || !cobj.contains_point(bind_pos)) {valid = 0; return 0;} // check status and also containment, in case coll id was reused
	return 1;
}


// *** LIGHT_SOURCE IMPLEMENTATION ***


// radius == 0.0 is really radius == infinity (no attenuation)
light_source::light_source(float sz, point const &p, point const &p2, colorRGBA const &c, bool id, vector3d const &d, float bw, float ri) :
	dynamic(id), enabled(1), radius(sz), radius_inv((radius == 0.0) ? 0.0 : 1.0/radius),
	r_inner(ri), bwidth(bw), pos(p), pos2(p2), dir(d.get_norm()), color(c), smap_data(nullptr)
{
	assert(bw > 0.0 && bw <= 1.0);
	assert(r_inner <= radius);
	assert(!(is_directional() && is_line_light())); // can't be both
}


void light_source::add_color(colorRGBA const &c) {

	color = color*color.alpha + c*c.alpha;
	color.alpha = 1.0;
}


float light_source::get_intensity_at(point const &p, point &updated_lpos) const {

	if (radius == 0.0) return color[3]; // no falloff
	updated_lpos = pos;

	if (is_line_light()) {
		vector3d const L(pos2 - pos);
		updated_lpos += L*CLIP_TO_01(dot_product((p - pos), L)/L.mag_sq());
	}
	if (fabs(p.z - updated_lpos.z) > radius) return 0.0; // fast test
	float const dist_sq(p2p_dist_sq(updated_lpos, p));
	if (dist_sq > radius*radius) return 0.0;
	float const rscale((radius - sqrt(dist_sq))*radius_inv);
	return rscale*rscale*color[3]; // quadratic 1/r^2 attenuation
}


float light_source::get_dir_intensity(vector3d const &obj_dir) const {

	if (!is_directional()) return 1.0;
	float const dp(dot_product(obj_dir, dir));
	if (dp >= 0.0 && (bwidth + LT_DIR_FALLOFF) < 0.5) return 0.0;
	float const dp_norm(0.5*(-dp*InvSqrt(obj_dir.mag_sq()) + 1.0)); // dp = -1.0 to 1.0, bw = 0.0 to 1.0
	return CLIP_TO_01(2.0f*(dp_norm + bwidth + LT_DIR_FALLOFF - 1.0f)*LT_DIR_FALLOFF_INV);
}


cube_t light_source::calc_bcube(float sqrt_thresh) const {

	assert(radius > 0.0);
	assert(sqrt_thresh < 1.0);
	cube_t bcube(pos, pos2);
	bcube.expand_by(radius*(1.0 - sqrt_thresh));

	if (is_very_directional()) {
		cube_t bcube2;
		calc_bounding_cylin(sqrt_thresh).calc_bcube(bcube2);
		bcube2.expand_by(vector3d(DX_VAL, DY_VAL, DZ_VAL)); // add one grid unit
		bcube.intersect_with_cube(bcube2);
	}
	return bcube;
}

void light_source::get_bounds(cube_t &bcube, int bnds[3][2], float sqrt_thresh, vector3d const &bounds_offset) const {

	if (radius == 0.0) { // global light source
		for (unsigned d = 0; d < 3; ++d) {
			bcube.d[d][0] = -SCENE_SIZE[d];
			bcube.d[d][1] =  SCENE_SIZE[d];
			bnds[d][0]    = 0;
			bnds[d][1]    = MESH_SIZE[d]-1;
		}
	}
	else { // local point/spot/line light source
		bcube = calc_bcube(sqrt_thresh);

		for (unsigned d = 0; d < 3; ++d) {
			UNROLL_2X(bnds[d][i_] = max(0, min(MESH_SIZE[d]-1, get_dim_pos((bcube.d[d][i_] + bounds_offset[d]), d)));)
		}
	}
}

float light_source::calc_cylin_end_radius() const {
	float const d(1.0 - 2.0*(bwidth + LT_DIR_FALLOFF));
	return radius*sqrt(1.0/(d*d) - 1.0);
}
cylinder_3dw light_source::calc_bounding_cylin(float sqrt_thresh) const {

	float const rad(radius*(1.0 - sqrt_thresh));
	if (is_line_light()) {return cylinder_3dw(pos, pos2, rad, rad);}
	assert(is_very_directional()); // not for use with point lights or spotlights larger than a hemisphere
	return cylinder_3dw(pos, pos+dir*rad, 0.0, (1.0 - sqrt_thresh)*calc_cylin_end_radius());
}


bool light_source::is_visible() const {

	if (!enabled) return 0;
	if (radius == 0.0) return 1;
	bool const line_light(is_line_light());
	
	if (line_light) {
		if (!camera_pdu.sphere_visible_test(0.5*(pos + pos2), (radius + 0.5*p2p_dist(pos, pos2)))) return 0; // use capsule bounding sphere
		if (!camera_pdu.cube_visible(calc_bcube())) return 0;
	}
	else {
		if (!camera_pdu.sphere_visible_test(pos, radius)) return 0; // view frustum culling
		if (is_very_directional() && !camera_pdu.cube_visible(calc_bcube())) return 0;
		if (radius < 0.5) return 1; // don't do anything more expensive for small light sources
		if (sphere_cobj_occluded(get_camera_pos(), pos, max(0.5f*radius, r_inner))) return 0; // approximate occlusion culling, can miss lights but rarely happens
	}
	if (dynamic || radius < 0.65 || !(display_mode & 0x08)) return 1; // dynamic lights (common case), small/medium lights, or occlusion culling disabled
	unsigned const num_rays = 100;
	static rand_gen_t rgen;
	static vector<vector3d> dirs;
	static map<pair<point, point>, point> ray_map;
	//shader_t shader;
	//shader.begin_color_only_shader(RED);
	//RESET_TIME;
	point const camera(get_camera_pos());
	int prev_cindex(-1);
	if (!check_coll_line_tree(pos, camera, prev_cindex, camera_coll_id, 0, 1, 1, 0, 1)) return 1; // light center is visible
	unsigned cur_dir(0);
	bool const directional(is_directional()), very_dir(is_very_directional());
	vector3d vortho[2];
	if (very_dir) {get_ortho_vectors(dir, vortho);}
	float const cylin_end_radius(very_dir ? calc_cylin_end_radius() : 0.0), radius_adj(radius*(1.0 - SQRT_CTHRESH));

	if (dirs.empty()) { // start with 26 uniformly distributed directions
		for (int x = -1; x <= 1; ++x) {
			for (int y = -1; y <= 1; ++y) {
				for (int z = -1; z <= 1; ++z) {
					if (x == 0 && y == 0 && z == 0) continue;
					dirs.push_back(vector3d(x, y, z).get_norm());
				}
			}
		}
	}
	for (unsigned n = 0; n < num_rays; ++n) { // for static scene lights we do ray queries
		vector3d ray_dir;
		point start_pos(pos);
		
		if (very_dir && n < num_rays/4) { // uniformly spaced around the cylinder perimeter
			float const theta(TWO_PI*float(n)/float(num_rays/4));
			ray_dir = radius*dir + cylin_end_radius*(SINF(theta)*vortho[0] + COSF(theta)*vortho[1]);
		}
		else if (directional) { // randomly spaced within cylinder volume
			while (1) {
				if (cur_dir >= dirs.size()) {dirs.push_back(rgen.signed_rand_vector_norm());}
				ray_dir = dirs[cur_dir++];
				if ((bwidth + LT_DIR_FALLOFF) < 0.5 && dot_product(ray_dir, dir) < 0.0) {ray_dir = -ray_dir;} // backwards
				if (get_dir_intensity(-ray_dir) > 0.0) break;
			}
		}
		else { // randomly spaced around the unit sphere
			if (cur_dir >= dirs.size()) {dirs.push_back(rgen.signed_rand_vector_norm());}
			ray_dir = dirs[cur_dir++];
			if (cur_dir > 26 && dir != zero_vector && dot_product(dir, ray_dir) < 0.0) {ray_dir = -ray_dir;} // invert direction
		}
		if (line_light) {start_pos += (float(n)/float(num_rays-1))*(pos2 - pos);} // fixed spacing along the length of the line
		point const end_pos(start_pos + radius_adj*ray_dir);
		pair<point, point> const key(start_pos, end_pos);
		auto it(ray_map.find(key));
		point cpos;
		int cindex(-1);
		
		if (it != ray_map.end()) {cpos = it->second;} // intersection point is cached
		else { // not found in cache, computer intersection point and add it
			vector3d cnorm; // unused
			if (check_coll_line_exact_tree(start_pos, end_pos, cpos, cnorm, cindex, camera_coll_id, 0, 1, 1, 0, 1)) {cpos -= SMALL_NUMBER*ray_dir;} // move away from coll pos
			else {cpos = end_pos;} // clamp to end_pos if no int
			if (cindex < 0 || coll_objects[cindex].truly_static()) {ray_map[key] = cpos;}
		}
		//draw_subdiv_sphere(cpos, 0.01, N_SPHERE_DIV/2, 0, 0);
		if (!camera_pdu.sphere_visible_test(cpos, 0.1*radius)) continue; // point not visible
		if ((prev_cindex < 0 || !coll_objects[prev_cindex].line_intersect(cpos, camera)) && // doesn't intersect the previous cobj
			!check_coll_line_tree(cpos, camera, cindex, camera_coll_id, 0, 1, 1, 0, 0)) return 1; // visible
		prev_cindex = cindex;
	}
	//shader.end_shader();
	//PRINT_TIME("Light Source Vis");
	return 0; // not visible
}


void light_source::combine_with(light_source const &l) { // Note: unused

	assert(radius > 0.0);
	float const w1(radius*radius*radius), w2(l.radius*l.radius*l.radius), wsum(w1 + w2), wa(w1/wsum), wb(w2/wsum);
	radius     = pow(wsum, (1.0f/3.0f));
	radius_inv = 1.0/radius;
	pos       *= wa;
	pos       += l.pos*wb; // weighted average
	blend_color(color, color, l.color, wa, 1);
}

bool light_source::try_merge_into(light_source &ls) const {

	if (ls.radius < radius) return 0; // shouldn't get here because of radius sort
	if (!dist_less_than(pos, ls.pos, 0.2*min(HALF_DXY, radius))) return 0;
	if (ls.bwidth != bwidth || ls.r_inner != r_inner || ls.dynamic != dynamic) return 0;
	if (is_directional() && dot_product(dir, ls.dir) < 0.95) return 0;
	if (is_line_light() || ls.is_line_light()) return 0; // don't merge line lights
	if (is_neg_light () != ls.is_neg_light ()) return 0; // don't merge neg lights (looks bad)
	colorRGBA lcolor(color);
	float const rr(radius/ls.radius);
	lcolor.alpha *= rr*rr; // scale by radius ratio squared
	ls.add_color(lcolor);
	return 1;
}

void light_source::pack_to_floatv(float *data) const {

	// store light_source as: {pos.xyz, radius}, {color.rgba}, {dir.xyz|pos2.xyz, bwidth}
	// Note: we don't really need to store the z-component of dir because we can calculate it from sqrt(1 - x*x - y*y),
	//       but doing this won't save us any texture data so it's not worth the trouble
	assert(data);
	UNROLL_3X(*(data++) = pos[i_];)
	*(data++) = radius;
	UNROLL_3X(*(data++) = 0.5*(1.0 + color[i_]);) // map [-1,1] => [0,1] for negative light support
	*(data++) = color[3];

	if (is_line_light()) {
		UNROLL_3X(*(data++) = pos2[i_];)
		*(data++) = 0.0; // pack bwidth as 0 to indicate a line light
	}
	else {
		UNROLL_3X(*(data++) = 0.5*(1.0 + dir[i_]);) // map [-1,1] to [0,1]
		*(data++) = bwidth; // [0,1]
	}
}

void light_source_trig::advance_timestep() {

	if (!bind_point_t::valid) {free_gl_state();} // free shadow map if invalid as an optimization
	if (!triggers.is_active()) return; // trigger not active
	enabled = (active_time > 0.0); // light on by default
	
	if (enabled) {
		if (triggers.get_auto_off_time() > 0.0) {active_time = max(0.0f, (active_time - fticks));} // decrease active time in auto off mode
	}
	else {
		if (triggers.get_auto_on_time()  > 0.0) {inactive_time += fticks;} // increase inactive time in auto on mode
	}
}

bool light_source_trig::check_activate(point const &p, float radius, int activator) {

	//if (active_time > 0.0) return 1; // already activated, don't reset timing
	float const auto_on_time(triggers.get_auto_on_time());
	unsigned trigger_mode(0); // 0 = not triggered, 1 bit = proximity, 2 bit = action, 4 bit = auto on
	if (auto_on_time > 0.0 && inactive_time > TICKS_PER_SECOND*auto_on_time) {inactive_time = 0.0; trigger_mode = 4;} // turn on, reset inactive_time
	trigger_mode |= triggers.register_player_pos(p, radius, activator, 1);
	if (trigger_mode == 0) return 0; // not yet triggered
	float const auto_off_time(triggers.get_auto_off_time());
	bool const is_off(active_time == 0.0);
	//if (auto_off_time == 0.0 || trigger.requires_action) {active_time = (is_off ? ((auto_off_time == 0.0) ? 1.0 : auto_off_time) : 0.0);} // toggle mode
	if (auto_off_time == 0.0)         {active_time = (is_off ? 1.0 : 0.0);} // toggle mode
	else if ((trigger_mode & 2) != 0) {active_time = (is_off ? auto_off_time : 0.0);} // toggle mode from user action with auto off
	else {active_time = auto_off_time;} // reset active time (on duration)
	active_time *= TICKS_PER_SECOND; // convert from seconds to ticks
	return 1;
}

void light_source_trig::check_shadow_map(unsigned tu_id) {

	if (is_line_light())    return; // line lights don't support shadow maps
	if (dir == zero_vector) return; // point light: need cube map, skip for now
	if (is_directional()) {} // directional vs. hemisphere: use 2D shadow map for both
	if (!is_enabled())      return; // disabled or destroyed
	if (!smap_data) {smap_data = new local_smap_data_t(tu_id);}
	//smap_data->pdu = pos_dir_up(pos, dir, up, angle, 0.0001*radius, radius, 1.0, 1); // FIXME: calculate up and angle
	smap_data->create_shadow_map_for_light(pos, nullptr);
}

void light_source_trig::free_gl_state() { // free shadow maps
	if (smap_data) {smap_data->free_gl_state(); delete smap_data;}
}


template<typename T> void shift_ls_vect(T &v, vector3d const &vd) {
	for (auto i = v.begin(); i != v.end(); ++i) {i->shift_by(vd);}
}
void shift_light_sources(vector3d const &vd) {
	shift_ls_vect(light_sources_a, vd);
	shift_ls_vect(light_sources_d, vd);
}

void free_light_source_gl_state() { // free shadow maps
	for (auto i = light_sources_d.begin(); i != light_sources_d.end(); ++i) {i->free_gl_state();}
}


