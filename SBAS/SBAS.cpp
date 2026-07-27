#include"SBAS.h"
#include"..\include\Utils.h"
#include"..\include\FormatConversion.h"
#include"..\include\ComplexMat.h"
#include"..\include\Filter.h"

namespace {
bool is_cancelled(IsCancelledCallback callback, void* context) noexcept {
	return callback != nullptr && callback(context);
}

void report_progress(InSARProgressCallback callback, void* context, int progress,
	const char* message) noexcept {
	if (callback != nullptr) {
		callback(context, progress, message);
	}
}
}
#ifdef _DEBUG
#pragma comment(lib, "ComplexMat_d.lib")
#pragma comment(lib, "Utils_d.lib")
#pragma comment(lib, "FormatConversion_d.lib")
#pragma comment(lib, "Filter_d.lib")
#else
#pragma comment(lib, "ComplexMat.lib")
#pragma comment(lib, "Utils.lib")
#pragma comment(lib, "FormatConversion.lib")
#pragma comment(lib, "Filter.lib")
#endif // _DEBUG

#define GET_NEXT_LINE \
{ \
	if( !fgets( instring, 256, fp ) ) \
		ch = 0; \
	else \
		ch = *instring; \
}



SBAS_node::SBAS_node()
{
	this->B_spatial = 0.0;
	this->B_temporal = 0.0;
	this->b_unwrapped = false;
	this->x = 0;
	this->y = 0;
	this->deformation_vel = 0.0;
	this->epsilon_height = 0.0;
	this->num_neigh_edges = 0;
	this->phase = 0.0;
}

SBAS_node::SBAS_node(int num_neigh_edge)
{
	this->B_spatial = 0.0;
	this->B_temporal = 0.0;
	this->b_unwrapped = false;
	this->y = 0;
	this->x = 0;
	this->deformation_vel = 0.0;
	this->epsilon_height = 0.0;
	this->num_neigh_edges = num_neigh_edge;
	this->phase = 0.0;
	this->neigh_edges.assign(num_neigh_edge, -1);
}

SBAS::SBAS()
{
	this->error_head = "SBAS_DLL_ERROR: error happens when using ";
}

SBAS::~SBAS()
{
}

int SBAS::write_spatialTemporal_node(
	const char* nodeFile, 
	const Mat& B_temporal,
	const Mat& B_effect
)
{
	if (!nodeFile ||
		B_temporal.rows != 1 ||
		B_temporal.cols < 2 ||
		B_effect.rows != 1 ||
		B_effect.cols != B_temporal.cols ||
		B_effect.type() != CV_64F ||
		B_temporal.type() != CV_64F
		)
	{
		fprintf(stderr, "write_spatialTemporal_node(): input check failed!\n");
		return -1;
	}
	double B_span, T_span, B_min, B_max, T_min, T_max;
	cv::minMaxLoc(B_temporal, &T_min, &T_max);
	cv::minMaxLoc(B_effect, &B_min, &B_max);
	B_span = B_max - B_min; 
	T_span = T_max - T_min;
	if (B_span < 1e-10 || T_span < 1e-10)
	{
		fprintf(stderr, "write_spatialTemporal_node(): B_span < 1e-10 || T_span < 1e-10!\n");
		return -1;
	}
	FILE* fp = NULL;
	fp = fopen(nodeFile, "wt");
	if (fp == NULL)
	{
		fprintf(stderr, "write_spatialTemporal_node(): can't open %s\n", nodeFile);
		return -1;
	}
	int nonzero = B_temporal.cols;
	int dim, attr1, attr2, count;
	dim = 2;
	attr1 = 0;
	attr2 = 0;
	fprintf(fp, "%d %d %d %d\n", nonzero, dim, attr1, attr2);
	int cols = B_temporal.cols;
	count = 1;
	double x, y;
	for (int i = 0; i < cols; i++)
	{
		x = (B_temporal.at<double>(0, i) - T_min) / T_span;
		y = (B_effect.at<double>(0, i) - B_min) / B_span;
		fprintf(fp, "%d %lf %lf\n", count, x, y);
		count++;
	}
	if (fp)
	{
		fclose(fp);
		fp = NULL;
	}
	return 0;
}

int SBAS::set_spatialTemporalBaseline(
	vector<SBAS_node>& nodes,
	const Mat& B_temporal,
	const Mat& B_effect
)
{
	int num_nodes = static_cast<int>(nodes.size());
	if (num_nodes != B_effect.cols ||
		B_temporal.rows != 1 ||
		B_temporal.cols < 2 ||
		B_effect.rows != 1 ||
		B_effect.cols != B_temporal.cols ||
		B_effect.type() != CV_64F ||
		B_temporal.type() != CV_64F
		)
	{
		fprintf(stderr, "set_spatialTemporalBaseline(): input check failed!\n");
		return -1;
	}
	double B_span, T_span, B_min, B_max, T_min, T_max;
	cv::minMaxLoc(B_temporal, &T_min, &T_max);
	cv::minMaxLoc(B_effect, &B_min, &B_max);
	B_span = B_max - B_min;
	T_span = T_max - T_min;
	if (B_span < 1e-10 || T_span < 1e-10)
	{
		fprintf(stderr, "set_spatialTemporalBaseline(): B_span < 1e-10 || T_span < 1e-10!\n");
		return -1;
	}
	for (int i = 0; i < num_nodes; i++)
	{
		nodes[i].B_spatial = B_effect.at<double>(0, i);
		nodes[i].B_temporal = B_temporal.at<double>(0, i);
		nodes[i].x = (B_temporal.at<double>(0, i) - T_min) / T_span;
		nodes[i].y = (B_effect.at<double>(0, i) - B_min) / B_span;
	}
	return 0;
}

int SBAS::read_edges(
	const char* edge_file,
	int num_nodes,
	vector<SBAS_edge>& edges,
	vector<int>& node_neighbours
)
{
	if (edge_file == NULL ||
		num_nodes < 3)
	{
		fprintf(stderr, "read_edges(): input check failed!\n");
		return -1;
	}
	FILE* fp = NULL;
	fp = fopen(edge_file, "rt");
	if (fp == NULL)
	{
		fprintf(stderr, "read_edges(): can't open %s\n", edge_file);
		return -1;
	}
	char str[1024];
	char* ptr;
	fgets(str, 1024, fp);
	long long num_edges = 0;
	num_edges = strtol(str, &ptr, 0);
	if (num_edges <= 0)
	{
		fprintf(stderr, "read_edges(): %s is unknown format!\n", edge_file);
		if (fp)
		{
			fclose(fp);
			fp = NULL;
		}
		return -1;
	}
	edges.clear(); node_neighbours.clear();
	edges.resize(num_edges);
	node_neighbours.resize(num_nodes);
	for (int i = 0; i < num_nodes; i++)
	{
		node_neighbours[i] = 0;
	}
	long end1, end2, edges_number, boundry_marker;
	for (int i = 0; i < num_edges; i++)
	{
		fgets(str, 1024, fp);
		edges_number = strtol(str, &ptr, 0);
		end1 = strtol(ptr, &ptr, 0);
		end2 = strtol(ptr, &ptr, 0);
		boundry_marker = strtol(ptr, &ptr, 0);
		edges[i].end1 = end1;
		edges[i].end2 = end2;
		edges[i].num = i + 1;
		edges[i].isBoundary = (boundry_marker == 1);
		if (end1 < 1 ||
			end1 > num_nodes ||
			end2 < 1 ||
			end2 > num_nodes)
		{
			fprintf(stderr, "read_edges(): endpoints exceed 1~num_nodes!\n");
			if (fp)
			{
				fclose(fp);
				fp = NULL;
			}
			return -1;
		}
		node_neighbours[end1 - 1] += 1;//统计每个节点有多少邻接边
		node_neighbours[end2 - 1] += 1;//统计每个节点有多少邻接边
	}
	if (fp)
	{
		fclose(fp);
		fp = NULL;
	}
	return 0;
}

int SBAS::init_SBAS_node(
	vector<SBAS_node>& node_array,
	vector<SBAS_edge>& edges,
	vector<int>& node_neighbours
)
{
	if (edges.size() < 3 ||
		node_neighbours.size() < 3)
	{
		fprintf(stderr, "init_SBAS_node(): input check failed!\n");
		return -1;
	}
	int num_nodes = static_cast<int>(node_neighbours.size());
	int num_edges = static_cast<int>(edges.size());
	node_array.clear();
	//node_array.resize(num_nodes);
	
	for (int i = 0; i < num_nodes; i++)
	{
		SBAS_node tmp(node_neighbours[i]);
		node_array.push_back(tmp);
	}

	SBAS_edge tmp;
	for (int i = 0; i < num_edges; i++)
	{
		tmp = edges[i];
		auto& neigh1 = node_array[tmp.end1 - 1].neigh_edges;
		for (size_t j = 0; j < neigh1.size(); j++)
		{
			if (neigh1[j] == -1)
			{
				neigh1[j] = i + 1;
				break;
			}
		}

		auto& neigh2 = node_array[tmp.end2 - 1].neigh_edges;
		for (size_t j = 0; j < neigh2.size(); j++)
		{
			if (neigh2[j] == -1)
			{
				neigh2[j] = i + 1;
				break;
			}
		}
	}
	return 0;
}

int SBAS::init_SBAS_triangle(
	const char* ele_file,
	const char* neigh_file,
	vector<SBAS_triangle>& triangle,
	vector<SBAS_edge>& edges,
	vector<SBAS_node>& nodes
)
{
	if (!ele_file ||
		!neigh_file ||
		edges.size() < 3 ||
		nodes.size() < 3)
	{
		fprintf(stderr, "init_SBAS_triangle(): input check failed!\n");
		return -1;
	}
	FILE* fp_ele, * fp_neigh;
	fp_ele = NULL;
	fp_neigh = NULL;
	fp_ele = fopen(ele_file, "rt");
	if (fp_ele == NULL)
	{
		fprintf(stderr, "init_SBAS_triangle(): can't open %s\n", ele_file);
		return -1;
	}
	fp_neigh = fopen(neigh_file, "rt");
	if (fp_neigh == NULL)
	{
		fprintf(stderr, "init_SBAS_triangle(): can't open %s\n", neigh_file);
		if (fp_ele)
		{
			fclose(fp_ele);
			fp_ele = NULL;
		}
		return -1;
	}

	char str[1024];
	char* ptr;
	fgets(str, 1024, fp_ele);
	int num_triangle = strtol(str, &ptr, 0);
	if (num_triangle < 1)
	{
		fprintf(stderr, "init_SBAS_triangle(): number of triangles exceed legal range!\n");
		if (fp_ele)
		{
			fclose(fp_ele);
			fp_ele = NULL;
		}
		if (fp_neigh)
		{
			fclose(fp_neigh);
			fp_neigh = NULL;
		}
		return -1;
	}
	fgets(str, 1024, fp_neigh);
	triangle.clear();
	triangle.resize(num_triangle);

	int p1, p2, p3, neigh1, neigh2, neigh3, num1, num2;
	for (int i = 0; i < num_triangle; i++)
	{
		fgets(str, 1024, fp_ele);
		num1 = strtol(str, &ptr, 0);
		p1 = strtol(ptr, &ptr, 0);
		p2 = strtol(ptr, &ptr, 0);
		p3 = strtol(ptr, &ptr, 0);

		fgets(str, 1024, fp_neigh);
		num2 = strtol(str, &ptr, 0);
		neigh1 = strtol(ptr, &ptr, 0);
		neigh2 = strtol(ptr, &ptr, 0);
		neigh3 = strtol(ptr, &ptr, 0);
		triangle[i].p1 = p1;
		triangle[i].p2 = p2;
		triangle[i].p3 = p3;
		triangle[i].neigh1 = neigh1;
		triangle[i].neigh2 = neigh2;
		triangle[i].neigh3 = neigh3;
		triangle[i].num = num1;
	}
	if (fp_ele)
	{
		fclose(fp_ele);
		fp_ele = NULL;
	}
	if (fp_neigh)
	{
		fclose(fp_neigh);
		fp_neigh = NULL;
	}
	//获取三角形的边序号
	int edge[3];
	memset(edge, 0, sizeof(int) * 3);
	for (int j = 0; j < num_triangle; j++)
	{
		const auto& neigh1 = nodes[triangle[j].p1 - 1].neigh_edges;
		for (size_t i = 0; i < neigh1.size(); i++)
		{
			if ((edges[neigh1[i] - 1].end1 == triangle[j].p2) ||
				(edges[neigh1[i] - 1].end2 == triangle[j].p2)
				)
			{
				edge[0] = neigh1[i];//确保edges1在p1和p2之间
			}
			if ((edges[neigh1[i] - 1].end1 == triangle[j].p3) ||
				(edges[neigh1[i] - 1].end2 == triangle[j].p3))
			{
				edge[2] = neigh1[i];//确保edges3在p1和p3之间
			}
		}
		const auto& neigh2 = nodes[triangle[j].p2 - 1].neigh_edges;
		for (size_t i = 0; i < neigh2.size(); i++)
		{
			if ((edges[neigh2[i] - 1].end1 == triangle[j].p3) ||
				(edges[neigh2[i] - 1].end2 == triangle[j].p3))
			{
				edge[1] = neigh2[i];//确保edges2在p2和p3之间
			}
		}
		triangle[j].edge1 = edge[0];
		triangle[j].edge2 = edge[1];
		triangle[j].edge3 = edge[2];
	}

	return 0;
}

static int compute_residue_common(
	std::vector<SBAS_node>& nodes,
	std::vector<SBAS_edge>& edges,
	std::vector<SBAS_triangle>& triangles,
	bool is_spatial_temporal
)
{
	if (nodes.size() < 3 ||
		edges.size() < 3 ||
		triangles.size() < 1)
	{
		return -1;
	}
	int num_triangle = static_cast<int>(triangles.size());
	for (int i = 0; i < num_triangle; i++)
	{
		int end1 = triangles[i].p1;
		int end2 = triangles[i].p2;
		int end3 = triangles[i].p3;
		double x1 = nodes[end1 - 1].x;
		double y1 = nodes[end1 - 1].y;
		double x2 = nodes[end2 - 1].x;
		double y2 = nodes[end2 - 1].y;
		double x3 = nodes[end3 - 1].x;
		double y3 = nodes[end3 - 1].y;

		double x21 = x2 - x1;
		double y21 = y2 - y1;
		double x32 = x3 - x2;
		double y32 = y3 - y2;

		double direction = x21 * y32 - x32 * y21;

		double res = 0.0;
		if (is_spatial_temporal)
		{
			double delta = 0.0;
			//由于edge1处于end1和end2之间
			if (nodes[end2 - 1].B_temporal > nodes[end1 - 1].B_temporal)
			{
				delta += edges[triangles[i].edge1 - 1].phase_gradient;
			}
			else
			{
				delta -= edges[triangles[i].edge1 - 1].phase_gradient;
			}
			//由于edge1处于end2和end3之间
			if (nodes[end3 - 1].B_temporal > nodes[end2 - 1].B_temporal)
			{
				delta += edges[triangles[i].edge2 - 1].phase_gradient;
			}
			else
			{
				delta -= edges[triangles[i].edge2 - 1].phase_gradient;
			}
			//edge3处于end1和end3之间
			if (nodes[end1 - 1].B_temporal > nodes[end3 - 1].B_temporal)
			{
				delta += edges[triangles[i].edge3 - 1].phase_gradient;
			}
			else
			{
				delta -= edges[triangles[i].edge3 - 1].phase_gradient;
			}

			res = round(delta / 2.0 / PI);
		}
		else
		{
			res += atan2(sin(nodes[end2 - 1].phase - nodes[end1 - 1].phase), cos(nodes[end2 - 1].phase - nodes[end1 - 1].phase));
			res += atan2(sin(nodes[end3 - 1].phase - nodes[end2 - 1].phase), cos(nodes[end3 - 1].phase - nodes[end2 - 1].phase));
			res += atan2(sin(nodes[end1 - 1].phase - nodes[end3 - 1].phase), cos(nodes[end1 - 1].phase - nodes[end3 - 1].phase));
			res = res / 2.0 / PI;
		}

		if (direction < 0.0)//在目标三角形中逆残差方向(残差积分方向定义为逆时针方向)
		{
			triangles[i].residue = -res;
		}
		else
		{
			triangles[i].residue = res;
		}
	}
	return 0;
}

int SBAS::compute_spatialTemporal_residue(
	vector<SBAS_node>& nodes,
	vector<SBAS_edge>& edges,
	vector<SBAS_triangle>& triangles
)
{
	if (nodes.size() < 3 ||
		edges.size() < 3 ||
		triangles.size() < 1)
	{
		fprintf(stderr, "compute_spatialTemporal_residue(): input check failed!\n");
		return -1;
	}
	return compute_residue_common(nodes, edges, triangles, true);
}

static double get_boundary_edge_weight(const SBAS_triangle& t, const std::vector<SBAS_edge>& edges, bool is_spatial)
{
	if (!is_spatial) return 1.0;
	if (edges[t.edge1 - 1].isBoundary) return edges[t.edge1 - 1].weight;
	if (edges[t.edge2 - 1].isBoundary) return edges[t.edge2 - 1].weight;
	return edges[t.edge3 - 1].weight;
}

static double get_neigh_edge_weight(const SBAS_triangle& t1, const SBAS_triangle& t2, const std::vector<SBAS_edge>& edges, bool is_spatial)
{
	if (!is_spatial) return 1.0;
	if (t2.edge1 == t1.edge1 || t2.edge1 == t1.edge2 || t2.edge1 == t1.edge3)
		return edges[t2.edge1 - 1].weight;
	if (t2.edge2 == t1.edge1 || t2.edge2 == t1.edge2 || t2.edge2 == t1.edge3)
		return edges[t2.edge2 - 1].weight;
	return edges[t2.edge3 - 1].weight;
}

static int writeDIMACS_common(
	const char* DIMACS_file,
	std::vector<SBAS_node>& nodes,
	std::vector<SBAS_edge>& edges,
	std::vector<SBAS_triangle>& triangle,
	bool is_spatial
)
{
	if (DIMACS_file == NULL ||
		triangle.size() < 1 ||
		nodes.size() < 3 ||
		edges.size() < 3
		)
	{
		fprintf(stderr, "writeDIMACS(): input check failed!\n\n");
		return -1;
	}
	FILE* fp = NULL;
	fp = fopen(DIMACS_file, "wt");
	if (fp == NULL)
	{
		fprintf(stderr, "writeDIMACS(): can't open %s\n", DIMACS_file);
		return -1;
	}

	int num_triangle = static_cast<int>(triangle.size());
	int num_arcs = 0;
	for (int i = 0; i < num_triangle; i++)
	{
		if (triangle[i].neigh1 > 0) num_arcs++;
		if (triangle[i].neigh2 > 0) num_arcs++;
		if (triangle[i].neigh3 > 0) num_arcs++;
	}

	//统计正负残差点并写入节点信息
	int positive = 0;
	int negative = 0;
	double thresh = 0.7;
	for (int i = 0; i < num_triangle; i++)
	{
		if (triangle[i].residue > thresh)
		{
			positive++;
		}
		if (triangle[i].residue < -thresh)
		{
			negative++;
		}
	}
	bool b_balanced = (positive == negative);
	if (negative == 0 && positive == 0)
	{
		if (fp) fclose(fp);
		fprintf(stderr, "writeDIMACS(): no residue point!\n\n");
		return -1;
	}
	fprintf(fp, "c This is MCF problem file.\n");
	fprintf(fp, "c Problem line(nodes, links)\n");
	//统计边缘三角形个数
	int boundry_tri = 0;
	for (int i = 0; i < num_triangle; i++)
	{
		if (edges[triangle[i].edge1 - 1].isBoundary ||
			edges[triangle[i].edge2 - 1].isBoundary ||
			edges[triangle[i].edge3 - 1].isBoundary)
		{
			boundry_tri++;
		}
	}
	
	bool treat_unbalanced = is_spatial ? true : !b_balanced;
	int n;
	if (treat_unbalanced)
	{
		n = num_triangle + 1;
		fprintf(fp, "p min %ld %ld\n", n, num_arcs + boundry_tri * 2);
	}
	else
	{
		n = num_triangle;
		fprintf(fp, "p min %ld %ld\n", n, num_arcs);
	}
	fprintf(fp, "c Node descriptor lines\n");
	double sum = 0.0;
	for (int i = 0; i < num_triangle; i++)
	{
		if (triangle[i].residue > thresh)
		{
			fprintf(fp, "n %d %lf\n", i + 1, triangle[i].residue);
			sum += triangle[i].residue;
		}
		if (triangle[i].residue < -thresh)
		{
			fprintf(fp, "n %d %lf\n", i + 1, triangle[i].residue);
			sum += triangle[i].residue;
		}
	}
	//写入大地节点
	if (treat_unbalanced)
	{
		fprintf(fp, "n %d %lf\n", num_triangle + 1, -sum);
	}

	//写入流费用
	fprintf(fp, "c Arc descriptor lines(from, to, minflow, maxflow, cost)\n");
	int lower_bound = 0;
	int upper_bound = 5;
	for (int i = 0; i < num_triangle; i++)
	{
		if (triangle[i].neigh1 > 0)
		{
			double cost_mean = get_neigh_edge_weight(triangle[i], triangle[triangle[i].neigh1 - 1], edges, is_spatial);
			fprintf(fp, "a %d %d %d %d %lf\n", i + 1, triangle[i].neigh1, lower_bound, upper_bound, cost_mean);
		}
		if (triangle[i].neigh2 > 0)
		{
			double cost_mean = get_neigh_edge_weight(triangle[i], triangle[triangle[i].neigh2 - 1], edges, is_spatial);
			fprintf(fp, "a %d %d %d %d %lf\n", i + 1, triangle[i].neigh2, lower_bound, upper_bound, cost_mean);
		}
		if (triangle[i].neigh3 > 0)
		{
			double cost_mean = get_neigh_edge_weight(triangle[i], triangle[triangle[i].neigh3 - 1], edges, is_spatial);
			fprintf(fp, "a %d %d %d %d %lf\n", i + 1, triangle[i].neigh3, lower_bound, upper_bound, cost_mean);
		}
	}
	if (treat_unbalanced)
	{
		//写入边界流费用
		for (int i = 0; i < num_triangle; i++)
		{
			if (edges[triangle[i].edge1 - 1].isBoundary ||
				edges[triangle[i].edge2 - 1].isBoundary ||
				edges[triangle[i].edge3 - 1].isBoundary)
			{
				double cost_mean = get_boundary_edge_weight(triangle[i], edges, is_spatial);
				fprintf(fp, "a %d %d %d %d %lf\n", i + 1, num_triangle + 1, lower_bound, upper_bound, cost_mean);
				fprintf(fp, "a %d %d %d %d %lf\n", num_triangle + 1, i + 1, lower_bound, upper_bound, cost_mean);
			}
		}
	}
	if (fp) fclose(fp);
	fp = NULL;
	return 0;
}

int SBAS::writeDIMACS_temporal(
	const char* DIMACS_file,
	vector<SBAS_node>& nodes,
	vector<SBAS_edge>& edges,
	vector<SBAS_triangle>& triangle
)
{
	return writeDIMACS_common(DIMACS_file, nodes, edges, triangle, false);
}

int SBAS::generate_interferograms(
	vector<string>& SLCH5Files,
	vector<SBAS_edge>& edges, 
	vector<SBAS_node>& nodes,
	int multilook_az, 
	int multilook_rg, 
	const char* ifgSavePath,
	bool b_save_images,
	IsCancelledCallback is_cancelled_callback,
	void* cancel_context,
	InSARProgressCallback progress_callback,
	void* progress_context
)
{
	if (edges.size() < 3 ||
		nodes.size() < 3 ||
		multilook_rg < 1 ||
		multilook_az < 1 ||
		SLCH5Files.size() != nodes.size() ||
		!ifgSavePath
		)
	{
		fprintf(stderr, "generate_interferograms(): input check failed!\n");
		return -1;
	}
	int ret;
	Utils util; FormatConversion conversion;
	string path(ifgSavePath), h5file;
	std::replace(path.begin(), path.end(), '/', '\\');
	ComplexMat master, slave;
	Mat phase;
	char str[256];
	int master_ix, slave_ix, offset_row, offset_col;
	double B_temporal, B_spatial;
	int num_edges = static_cast<int>(edges.size());
	for (int i = 0; i < num_edges; i++)
	{
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		report_progress(progress_callback, progress_context,
			i * 100 / std::max(1, num_edges), "Generating interferograms...");
		if (nodes[edges[i].end1 - 1].B_temporal > nodes[edges[i].end2 - 1].B_temporal)
		{
			master_ix = edges[i].end1;slave_ix = edges[i].end2;
			B_temporal = nodes[edges[i].end1 - 1].B_temporal - nodes[edges[i].end2 - 1].B_temporal;
			B_spatial = nodes[edges[i].end1 - 1].B_spatial - nodes[edges[i].end2 - 1].B_spatial;
		}
		else
		{
			master_ix = edges[i].end2; slave_ix = edges[i].end1;
			B_temporal = nodes[edges[i].end2 - 1].B_temporal - nodes[edges[i].end1 - 1].B_temporal;
			B_spatial = nodes[edges[i].end2 - 1].B_spatial - nodes[edges[i].end1 - 1].B_spatial;
		}
		ret = conversion.read_slc_from_h5(SLCH5Files[master_ix - 1].c_str(), master);
		if (return_check(ret, "read_slc_from_h5()", error_head)) return -1;
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		ret = conversion.read_int_from_h5(SLCH5Files[master_ix - 1].c_str(), "offset_row", &offset_row);
		if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		ret = conversion.read_int_from_h5(SLCH5Files[master_ix - 1].c_str(), "offset_col", &offset_col);
		if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		ret = conversion.read_slc_from_h5(SLCH5Files[slave_ix - 1].c_str(), slave);
		if (return_check(ret, "read_slc_from_h5()", error_head)) return -1;
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		if (master.type() != CV_64F) master.convertTo(master, CV_64F);
		if (slave.type() != CV_64F) slave.convertTo(slave, CV_64F);
		ret = util.Multilook(master, slave, multilook_rg, multilook_az, phase);
		if (return_check(ret, "Multilook()", error_head)) return -1;
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		snprintf(str, sizeof(str), "\\%d.h5", i + 1);
		h5file = path + str;
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		ret = conversion.validate_distinct_h5_output(SLCH5Files[master_ix - 1].c_str(), h5file.c_str());
		if (return_check(ret, "validate_distinct_h5_output()", error_head)) return -1;
		ret = conversion.creat_new_h5(h5file.c_str());
		if (return_check(ret, "creat_new_h5()", error_head)) return -1;
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		ret = conversion.write_array_to_h5(h5file.c_str(), "phase", phase);
		if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		ret = conversion.write_int_to_h5(h5file.c_str(), "offset_row", offset_row);
		if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
		ret = conversion.write_int_to_h5(h5file.c_str(), "offset_col", offset_col);
		if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
		ret = conversion.write_int_to_h5(h5file.c_str(), "range_len", phase.cols);
		if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
		ret = conversion.write_int_to_h5(h5file.c_str(), "azimuth_len", phase.rows);
		if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
		ret = conversion.write_int_to_h5(h5file.c_str(), "multilook_rg", multilook_rg);
		if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
		ret = conversion.write_int_to_h5(h5file.c_str(), "multilook_az", multilook_az);
		if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
		ret = conversion.write_double_to_h5(h5file.c_str(), "B_temporal", B_temporal);
		if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
		ret = conversion.write_double_to_h5(h5file.c_str(), "B_spatial", B_spatial);
		if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
		ret = conversion.Copy_para_from_h5_2_h5(SLCH5Files[master_ix - 1].c_str(), h5file.c_str());
		if (return_check(ret, "Copy_para_from_h5_2_h5()", error_head)) return -1;
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		
		//是否保存为图片
		if (b_save_images)
		{
			if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
			sprintf(str, "\\%d.jpg", i + 1);
			h5file = path + str;
			ret = util.savephase(h5file.c_str(), "jet", phase);
			if (return_check(ret, "savephase()", error_head)) return -1;
		}

	}
	return 0;
}

int SBAS::writeDIMACS_spatial(
	const char* DIMACS_file,
	vector<SBAS_node>& nodes, 
	vector<SBAS_edge>& edges, 
	vector<SBAS_triangle>& triangle
)
{
	return writeDIMACS_common(DIMACS_file, nodes, edges, triangle, true);
}

int SBAS::saveGradientStack(
	vector<string>& phaseFiles,
	const Mat& mask, 
	vector<SBAS_node>& nodes, 
	vector<SBAS_edge>& edges,
	const char* dstH5File,
	IsCancelledCallback is_cancelled_callback,
	void* cancel_context,
	InSARProgressCallback progress_callback,
	void* progress_context
)
{
	if (phaseFiles.size() < 3 ||
		mask.type() != CV_32S ||
		nodes.size() < 3 ||
		edges.size() < 3 ||
		!dstH5File
		)
	{
		fprintf(stderr, "saveGradientStack(): input check failed!\n");
		return -1;
	}
	int num_ifgs = static_cast<int>(phaseFiles.size());
	int num_nodes = static_cast<int>(nodes.size());
	int num_edges = static_cast<int>(edges.size());
	FormatConversion conversion;
	int ret;
	char str[1024];
	Mat phase;
	Mat gradient = Mat::zeros(1, num_edges, CV_64F);
	ret = conversion.creat_new_h5(dstH5File);
	if (return_check(ret, "creat_new_h5()", error_head)) return -1;
	if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
	for (int i = 0; i < num_ifgs; i++)
	{
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		report_progress(progress_callback, progress_context,
			i * 100 / std::max(1, num_ifgs), "Saving gradient stack...");
		ret = conversion.read_array_from_h5(phaseFiles[i].c_str(), "phase", phase);
		if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		ret = set_high_coherence_node_phase(mask, nodes, edges, phase);
		if (return_check(ret, "set_high_coherence_node_phase()", error_head)) return -1;
		sprintf(str, "ifg_gradient_%d", i + 1);
#pragma omp parallel for schedule(guided)
		for (int j = 0; j < num_edges; j++)
		{
			if ((j & 0x3ff) == 0 &&
				is_cancelled(is_cancelled_callback, cancel_context)) continue;
			gradient.at<double>(0, j) = edges[j].phase_gradient;
		}
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		ret = conversion.write_array_to_h5(dstH5File, str, gradient);
		if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
	}

	return 0;
}

int SBAS::readDIMACS(
	const char* DIMACS_file_solution,
	vector<SBAS_node>& nodes,
	vector<SBAS_edge>& edges,
	vector<SBAS_triangle>& triangle,
	double& obj_value,
	double* flowcount
)
{
	if (DIMACS_file_solution == NULL ||
		edges.size() < 3 ||
		nodes.size() < 3 ||
		triangle.size() < 1
		)
	{
		fprintf(stderr, "readDIMACS(): input check failed!\n\n");
		return -1;
	}
	FILE* fp = NULL;
	fp = fopen(DIMACS_file_solution, "rt");
	if (fp == NULL)
	{
		fprintf(stderr, "readDIMACS(): can't open %s \n", DIMACS_file_solution);
		return -1;
	}
	double flow_sum = 0.0;
	char instring[256];
	char ch;
	// removed unused: tmp, row1, col1, row2, col2, row3, col3 (copy-paste remnant from grid-based readDIMACS)
	int i, end1, end2, end3;
	// removed unused: end[3] (planned endpoint array, never used)
	// removed unused: x1, y1, x2, y2, x3, y3 (coordinates computed inline via node array access)
	double direction, x21, x32, y21, y32;
	long from, to;
	double flow = 0;
	// removed unused: flag, x[3], y[3] (planned neighbor iteration, never implemented)
	long* ptr_neigh = NULL;
	// removed unused: num_neigh (planned neighbor lookup, never used)
	int target_edges;
	int num_nodes = static_cast<int>(nodes.size());
	int num_triangle = static_cast<int>(triangle.size()); int num_edges = static_cast<int>(edges.size());
	/////////////////////读取注释///////////////////////////
	GET_NEXT_LINE;
	while (ch != 's' && ch)
	{
		if (ch != 'c')
		{
			if (fp) fclose(fp);
			fprintf(stderr, "readDIMACS(): unknown file format!\n\n");
			return -1;
		}
		GET_NEXT_LINE;
	}
	/////////////////////读取优化目标值/////////////////////
	for (i = 1; i < 81; i++)
	{
		if (isspace((int)instring[i]) > 0)
		{
			i++;
			break;
		}
	}
	if (sscanf(&(instring[i]), "%lf", &obj_value) != 1)
	{
		if (fp) fclose(fp);
		fprintf(stderr, "read_DIMACS(): unknown file format!\n\n");
		return -1;
	}
	if (obj_value < 0.0)
	{
		if (fp) fclose(fp);
		fprintf(stderr, "readDIMACS(): this problem can't be solved(unbounded or infeasible)!\n\n");
		return -1;
	}
	////////////////////////读取MCF结果////////////////////////
	GET_NEXT_LINE;
	while (ch && ch == 'f')
	{
		if (sscanf(&(instring[2]), "%ld %ld %lf", &from, &to, &flow) != 3 ||
			flow < 0.0 || from < 0 || to < 0)
		{
			if (fp) fclose(fp);
			fprintf(stderr, "readDIMACS(): unknown file format!\n\n");
			return -1;
		}
		flow_sum += fabs(flow);
		/*
		* 非接地边
		*/
		if (from > 0 &&
			from <= num_triangle &&
			to > 0 &&
			to <= num_triangle)
		{
			/*
			* 寻找from/to两个三角形的公共边
			*/
			if (triangle[to - 1].edge1 == triangle[from - 1].edge1 ||
				triangle[to - 1].edge1 == triangle[from - 1].edge2 ||
				triangle[to - 1].edge1 == triangle[from - 1].edge3
				)
			{
				target_edges = triangle[to - 1].edge1;
			}
			else if (triangle[to - 1].edge2 == triangle[from - 1].edge1 ||
				triangle[to - 1].edge2 == triangle[from - 1].edge2 ||
				triangle[to - 1].edge2 == triangle[from - 1].edge3
				)
			{
				target_edges = triangle[to - 1].edge2;
			}
			else
			{
				target_edges = triangle[to - 1].edge3;
			}

			end1 = edges[target_edges - 1].end1;
			end2 = edges[target_edges - 1].end2;
			if ((triangle[from - 1].p1 == end1 || triangle[from - 1].p1 == end2) &&
				(triangle[from - 1].p2 == end1 || triangle[from - 1].p2 == end2))
			{
				end3 = triangle[from - 1].p3;
			}
			else if ((triangle[from - 1].p1 == end1 || triangle[from - 1].p1 == end2) &&
				(triangle[from - 1].p3 == end1 || triangle[from - 1].p3 == end2))
			{
				end3 = triangle[from - 1].p2;
			}
			else
			{
				end3 = triangle[from - 1].p1;
			}
			x21 = nodes[end2 - 1].x - nodes[end1 - 1].x;
			y21 = nodes[end2 - 1].y - nodes[end1 - 1].y;
			x32 = nodes[end3 - 1].x - nodes[end2 - 1].x;
			y32 = nodes[end3 - 1].y - nodes[end2 - 1].y;

			direction = x21 * y32 - x32 * y21;

			//顺残差方向(end1--->end2梯度积分需要减去flow值)
			if (direction > 0)
			{
				//如果end1 > end2
				if (end1 > end2)
				{
					edges[target_edges - 1].phase_gradient += flow * PI * 2.0;
				}
				else
				{
					edges[target_edges - 1].phase_gradient -= flow * PI * 2.0;
				}
			}
			//逆残差方向(end1--->end2梯度积分需要加上flow值)
			else
			{
				//如果end1 > end2
				if (end1 > end2)
				{
					edges[target_edges - 1].phase_gradient -= flow * PI * 2.0;
				}
				else
				{
					edges[target_edges - 1].phase_gradient += flow * PI * 2.0;
				}
			}
		}

		/*
		* 接地边
		*/
		else
		{
			if (from == num_triangle + 1)
			{
				if (edges[triangle[to - 1].edge1 - 1].isBoundary)target_edges = triangle[to - 1].edge1;
				else if (edges[triangle[to - 1].edge2 - 1].isBoundary) target_edges = triangle[to - 1].edge2;
				else target_edges = triangle[to - 1].edge3;
				end1 = edges[target_edges - 1].end1;
				end2 = edges[target_edges - 1].end2;
				if (triangle[to - 1].p1 != end1 && triangle[to - 1].p1 != end2) end3 = triangle[to - 1].p1;
				else if (triangle[to - 1].p2 != end1 && triangle[to - 1].p2 != end2) end3 = triangle[to - 1].p2;
				else end3 = triangle[to - 1].p3;

				x21 = nodes[end2 - 1].x - nodes[end1 - 1].x;
				y21 = nodes[end2 - 1].y - nodes[end1 - 1].y;
				x32 = nodes[end3 - 1].x - nodes[end2 - 1].x;
				y32 = nodes[end3 - 1].y - nodes[end2 - 1].y;

				direction = x21 * y32 - x32 * y21;

				//顺残差方向(end1--->end2梯度积分需要加上flow值)
				if (direction > 0)
				{
					//如果end1 > end2
					if (end1 > end2)
					{
						edges[target_edges - 1].phase_gradient -= flow * PI * 2.0;
					}
					else
					{
						edges[target_edges - 1].phase_gradient += flow * PI * 2.0;
					}
				}
				//逆残差方向(end1--->end2梯度积分需要减去flow值)
				else
				{
					//如果end1 > end2
					if (end1 > end2)
					{
						edges[target_edges - 1].phase_gradient += flow * PI * 2.0;
					}
					else
					{
						edges[target_edges - 1].phase_gradient -= flow * PI * 2.0;
					}
				}

			}
			else
			{
				if (edges[triangle[from - 1].edge1 - 1].isBoundary)target_edges = triangle[from - 1].edge1;
				else if (edges[triangle[from - 1].edge2 - 1].isBoundary) target_edges = triangle[from - 1].edge2;
				else target_edges = triangle[from - 1].edge3;
				end1 = edges[target_edges - 1].end1;
				end2 = edges[target_edges - 1].end2;
				if (triangle[from - 1].p1 != end1 && triangle[from - 1].p1 != end2) end3 = triangle[from - 1].p1;
				else if (triangle[from - 1].p2 != end1 && triangle[from - 1].p2 != end2) end3 = triangle[from - 1].p2;
				else end3 = triangle[from - 1].p3;

				x21 = nodes[end2 - 1].x - nodes[end1 - 1].x;
				y21 = nodes[end2 - 1].y - nodes[end1 - 1].y;
				x32 = nodes[end3 - 1].x - nodes[end2 - 1].x;
				y32 = nodes[end3 - 1].y - nodes[end2 - 1].y;

				direction = x21 * y32 - x32 * y21;

				//顺残差方向(end1--->end2梯度积分需要减去flow值)
				if (direction > 0)
				{
					//如果end1 > end2
					if (end1 > end2)
					{
						edges[target_edges - 1].phase_gradient += flow * PI * 2.0;
					}
					else
					{
						edges[target_edges - 1].phase_gradient -= flow * PI * 2.0;
					}
				}
				//逆残差方向(end1--->end2梯度积分需要加上flow值)
				else
				{
					//如果end1 > end2
					if (end1 > end2)
					{
						edges[target_edges - 1].phase_gradient -= flow * PI * 2.0;
					}
					else
					{
						edges[target_edges - 1].phase_gradient += flow * PI * 2.0;
					}
				}
			}
		}

		GET_NEXT_LINE;
	}
	if (fp)
	{
		fclose(fp);
		fp = NULL;
	}
	if (flowcount) *flowcount = flow_sum;
	return 0;
}

int SBAS::generate_high_coherence_mask(
	vector<string>& phaseFiles, 
	int wndsize_rg,
	int wndsize_az, 
	double coherence_thresh,
	double count_thresh,
	Mat& mask,
	IsCancelledCallback is_cancelled_callback,
	void* cancel_context,
	InSARProgressCallback progress_callback,
	void* progress_context
)
{
	if (phaseFiles.size() < 1 ||
		wndsize_az % 2 != 1 ||
		wndsize_rg % 2 != 1
		)
	{
		fprintf(stderr, "generate_high_coherence_mask(): input check failed!\n");
		return -1;
	}
	FormatConversion conversion; Utils util;
	int ret;
	Mat phase, coherence;
	coherence_thresh = coherence_thresh < 0.3 ? 0.3 : coherence_thresh;
	coherence_thresh = coherence_thresh > 0.95 ? 0.95 : coherence_thresh;
	count_thresh = count_thresh < 0.3 ? 0.3 : count_thresh;
	count_thresh = count_thresh > 1.0 ? 1.0 : count_thresh;
	int num_images = static_cast<int>(phaseFiles.size());
	for (int i = 0; i < num_images; i++)
	{
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		report_progress(progress_callback, progress_context,
			i * 100 / std::max(1, num_images), "Generating high coherence mask...");
		ret = conversion.read_array_from_h5(phaseFiles[i].c_str(), "phase", phase);
		if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		int rows = phase.rows; int cols = phase.cols;
		ret = conversion.read_array_from_h5(phaseFiles[i].c_str(), "coherence", coherence);
		if (ret < 0)
		{
			ret = util.phase_coherence(phase, wndsize_rg, wndsize_az, coherence);
			if (return_check(ret, "phase_coherence()", error_head)) return -1;
			cv::Mat coherence_32f;
			coherence.convertTo(coherence_32f, CV_32F);
			ret = conversion.write_array_to_h5(phaseFiles[i].c_str(), "coherence", coherence_32f);
			if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
			if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		}
		if (coherence.type() != CV_64F)
		{
			coherence.convertTo(coherence, CV_64F);
		}
		
		if (i == 0)
		{
			Mat tmp = Mat::zeros(rows, cols, CV_32S);
			tmp.copyTo(mask);
		}
#pragma omp parallel for schedule(guided)
		for (int j = 0; j < rows; j++)
		{
			if (is_cancelled(is_cancelled_callback, cancel_context)) continue;
			for (int k = 0; k < cols; k++)
			{
				if (coherence.at<double>(j, k) > coherence_thresh) mask.at<int>(j, k) = mask.at<int>(j, k) + 1;
			}
		}
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
	}
	int rows = mask.rows; int cols = mask.cols;
	int count = static_cast<int>(num_images * count_thresh);
#pragma omp parallel for schedule(guided)
	for (int j = 0; j < rows; j++)
	{
		if (is_cancelled(is_cancelled_callback, cancel_context)) continue;
		for (int k = 0; k < cols; k++)
		{
			if (mask.at<int>(j, k) > count) mask.at<int>(j, k) = 1;
			else mask.at<int>(j, k) = 0;
		}
	}
	if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
	return 0;
}

int SBAS::floodFillUnwrap(
	vector<SBAS_node>& nodes, vector<SBAS_edge>& edges, int start, bool b_zero_start,
	IsCancelledCallback is_cancelled_callback, void* cancel_context,
	InSARProgressCallback progress_callback, void* progress_context)
{
	if (nodes.size() < 3 ||
		edges.size() < 3 ||
		start < 1 ||
		start > nodes.size())
	{
		fprintf(stderr, "floodFillUnwrap(): input check failed!\n");
		return -1;
	}
	int num_nodes = static_cast<int>(nodes.size());
	int num_edges = static_cast<int>(edges.size());
	int node_ix, end1, end2;
	double grad;
	queue<int> node_que;
	node_que.push(start);
	if (b_zero_start) nodes[start - 1].phase = 0.0;
	int completed = 0;
	int step = std::max(1, num_nodes / 100);
	while (!node_que.empty())
	{
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		node_ix = node_que.front();
		node_que.pop();
		nodes[node_ix - 1].b_unwrapped = true;
		const auto& neigh = nodes[node_ix - 1].neigh_edges;
		for (size_t i = 0; i < neigh.size(); i++)
		{
			end1 = edges[neigh[i] - 1].end1;
			end2 = edges[neigh[i] - 1].end2;
			grad = edges[neigh[i] - 1].phase_gradient;
			if (end1 == node_ix)
			{
				if (!nodes[end2 - 1].b_unwrapped)
				{
					nodes[end2 - 1].b_unwrapped = true;
					if (node_ix > end2)
					{
						nodes[end2 - 1].phase = nodes[node_ix - 1].phase - grad;
					}
					else 
					{
						nodes[end2 - 1].phase = nodes[node_ix - 1].phase + grad;
					}
					node_que.push(end2);
				}
			}
			else
			{
				if (!nodes[end1 - 1].b_unwrapped)
				{
					nodes[end1 - 1].b_unwrapped = true;
					if (node_ix > end1)
					{
						nodes[end1 - 1].phase = nodes[node_ix - 1].phase - grad;
					}
					else
					{
						nodes[end1 - 1].phase = nodes[node_ix - 1].phase + grad;
					}
					node_que.push(end1);
				}
			}
		}
		completed++;
		if (completed % step == 0)
		{
			report_progress(progress_callback, progress_context,
				completed * 100 / num_nodes, "Flood fill unwrapping...");
		}
	}
	return 0;
}

int SBAS::set_weight_by_coherence(const Mat& coherence, vector<SBAS_node>& nodes, vector<SBAS_edge>& edges)
{
	if ((coherence.type() != CV_64F && coherence.type() != CV_32F) ||
		nodes.size() < 3 ||
		edges.size() < 3)
	{
		fprintf(stderr, "set_weight_by_coherence(): input check failed!\n");
		return -1;
	}
	cv::Mat coh_64f = coherence;
	if (coh_64f.type() != CV_64F) coh_64f.convertTo(coh_64f, CV_64F);
	int num_nodes = static_cast<int>(nodes.size());
	int num_edges = static_cast<int>(edges.size());
	int rows = coh_64f.rows;
	int cols = coh_64f.cols;
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < num_edges; i++)
	{
		int row, col;
		double weight;

		weight = 0.0;
		row = rows - static_cast<int>(round(nodes[edges[i].end1 - 1].y));
		col = static_cast<int>(round(nodes[edges[i].end1 - 1].x)) - 1;
		weight += coh_64f.at<double>(row, col);

		row = rows - static_cast<int>(round(nodes[edges[i].end2 - 1].y));
		col = static_cast<int>(round(nodes[edges[i].end2 - 1].x)) - 1;
		weight += coh_64f.at<double>(row, col);

		weight = weight / 2.0;

		edges[i].weight = weight;
	}
	return 0;
}

int SBAS::retrieve_unwrapped_phase(vector<SBAS_node>& nodes, Mat& phase)
{
	if (phase.empty() ||
		phase.type() != CV_64F ||
		nodes.size() < 3)
	{
		fprintf(stderr, "retrieve_unwrapped_phase(): input check failed!\n");
		return -1;
	}
	int num_nodes = static_cast<int>(nodes.size());

	int rows = phase.rows;
	int cols = phase.cols;
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < num_nodes; i++)
	{
		int row, col;
		if (nodes[i].b_unwrapped)
		{
			row = rows - static_cast<int>(round(nodes[i].y));
			col = static_cast<int>(round(nodes[i].x)) - 1;
			phase.at<double>(row, col) = nodes[i].phase;
		}
	}
	return 0;
}

int SBAS::compute_high_coherence_residue(
	vector<SBAS_node>& nodes, 
	vector<SBAS_edge>& edges,
	vector<SBAS_triangle>& triangles
)
{
	if (nodes.size() < 3 ||
		edges.size() < 3 ||
		triangles.size() < 1)
	{
		fprintf(stderr, "compute_high_coherence_residue(): input check failed!\n");
		return -1;
	}
	return compute_residue_common(nodes, edges, triangles, false);
}

static int find_connecting_edge(const SBAS_triangle& t, int u, int v, const std::vector<SBAS_edge>& edges)
{
	int e1 = t.edge1;
	if ((edges[e1 - 1].end1 == u && edges[e1 - 1].end2 == v) || (edges[e1 - 1].end1 == v && edges[e1 - 1].end2 == u))
		return e1;
	int e2 = t.edge2;
	if ((edges[e2 - 1].end1 == u && edges[e2 - 1].end2 == v) || (edges[e2 - 1].end1 == v && edges[e2 - 1].end2 == u))
		return e2;
	return t.edge3;
}

static double get_step_gradient(int u, int v, const SBAS_triangle& t, const std::vector<SBAS_edge>& edges)
{
	int e = find_connecting_edge(t, u, v, edges);
	return (v > u ? 1.0 : -1.0) * edges[e - 1].phase_gradient;
}

int SBAS::compute_high_coherence_residue_by_gradient(
	vector<SBAS_node>& nodes, 
	vector<SBAS_edge>& edges,
	vector<SBAS_triangle>& triangles
)
{
	if (nodes.size() < 3 ||
		edges.size() < 3 ||
		triangles.size() < 1)
	{
		fprintf(stderr, "compute_high_coherence_residue_by_gradient(): input check failed!\n");
		return -1;
	}
	int num_triangle = static_cast<int>(triangles.size());
	int num_nodes = static_cast<int>(nodes.size());
	// removed unused: tmp (copy-paste remnant from readDIMACS)
	int end1, end2, end3;
	double x21, y21, x32, y32, direction, residue, x1, x2, x3, y1, y2, y3;
	residue = 0.0;
	for (int i = 0; i < num_triangle; i++)
	{
		end1 = triangles[i].p1;
		end2 = triangles[i].p2;
		end3 = triangles[i].p3;
		x1 = nodes[end1 - 1].x;
		y1 = nodes[end1 - 1].y;
		x2 = nodes[end2 - 1].x;
		y2 = nodes[end2 - 1].y;
		x3 = nodes[end3 - 1].x;
		y3 = nodes[end3 - 1].y;
		x21 = x2 - x1;
		y21 = y2 - y1;
		x32 = x3 - x2;
		y32 = y3 - y2;
		direction = x21 * y32 - x32 * y21;
		residue = 0.0;
		residue += get_step_gradient(end1, end2, triangles[i], edges);
		residue += get_step_gradient(end2, end3, triangles[i], edges);
		residue += get_step_gradient(end3, end1, triangles[i], edges);
		residue = round(residue / 2.0 / PI);

		if (direction < 0.0)//在目标三角形中逆残差方向(残差方向定义为逆时针方向)
		{
			triangles[i].residue = -residue;
		}
		else
		{
			triangles[i].residue = residue;
		}
	}
	return 0;
}

int SBAS::residue_num(vector<SBAS_triangle>& triangles, int* num)
{
	if (triangles.size() < 1 || !num)
	{
		fprintf(stderr, "residue_num(): input check failed!\n");
		return -1;
	}
	int number = 0;
	for (int i = 0; i < triangles.size(); i++)
	{
		if (fabs(triangles[i].residue) > 0.5) number++;
	}
	*num = number;
	return 0;
}

int SBAS::get_formation_matrix(
	const Mat& spatial,
	const Mat& temporal, 
	double spatial_thresh,
	double temporal_thresh_low,
	double temporal_thresh,
	Mat& formation_matrix,
	Mat& spatial_baseline,
	Mat& temporal_baseline
)
{
	if (spatial.type() != CV_64F ||
		temporal.type() != CV_64F ||
		spatial.rows != 1 ||
		spatial.cols < 2 ||
		temporal.rows != 1 ||
		spatial.cols != temporal.cols
		)
	{
		fprintf(stderr, "get_formation_matrix(): input check failed!\n");
		return -1;
	}
	int n = spatial.cols;
	formation_matrix.create(n, n, CV_32S);
	spatial_baseline.create(n, n, CV_64F);
	temporal_baseline.create(n, n, CV_64F);
	formation_matrix = 0;
	temporal_baseline = 0.0;
	spatial_baseline = 0.0;
	for (int i = 0; i < n; i++)
	{
		for (int j = 0; j < i; j++)
		{
			if (fabs(spatial.at<double>(0, j) - spatial.at<double>(0, i)) < spatial_thresh &&
				fabs((temporal.at<double>(0, j) - temporal.at<double>(0, i)) / 365.0) < temporal_thresh &&
				fabs((temporal.at<double>(0, j) - temporal.at<double>(0, i)) / 365.0) > temporal_thresh_low / 365.0
				)
			{
				formation_matrix.at<int>(i, j) = 1;
				spatial_baseline.at<double>(i, j) = spatial.at<double>(0, i) - spatial.at<double>(0, j);
				temporal_baseline.at<double>(i, j) = (temporal.at<double>(0, i) - temporal.at<double>(0, j)) / 365.0;
			}
		}
	}
	return 0;
}

int SBAS::generate_interferograms(
	vector<string>& SLCH5Files, 
	const Mat& formation_matrix, 
	const Mat& spatial_baseline,
	const Mat& temporal_baseline,
	int multilook_az,
	int multilook_rg,
	const char* ifgSavePath,
	bool b_save_images,
	double alpha,
	IsCancelledCallback is_cancelled_callback,
	void* cancel_context,
	InSARProgressCallback progress_callback,
	void* progress_context
)
{
	if (formation_matrix.type() != CV_32S ||
		formation_matrix.rows != formation_matrix.cols ||
		spatial_baseline.size() != formation_matrix.size() ||
		temporal_baseline.size() != formation_matrix.size() ||
		multilook_rg < 1 ||
		multilook_az < 1 ||
		(int)SLCH5Files.size() != formation_matrix.cols ||
		!ifgSavePath
		)
	{
		fprintf(stderr, "generate_interferograms(): input check failed!\n");
		return -1;
	}
	int ret;
	int n_images = formation_matrix.rows;
	Utils util; FormatConversion conversion; Filter filter;
	std::string path(ifgSavePath), h5file;
	std::replace(path.begin(), path.end(), '/', '\\');
	ComplexMat master, slave;
	Mat phase, coherence, mapped_lat, mapped_lon;
	char str[256];
	int master_ix, slave_ix, offset_row, offset_col;
	double B_temporal, B_spatial;
	bool b_mappedLatLon_written = false;
	int pair_count = 0;
	for (int i = 0; i < n_images; i++) for (int j = 0; j < i; j++) if (formation_matrix.at<int>(i, j) == 1) pair_count++;
	int current_pair = 0;
	for (int i = 0; i < n_images; i++)
	{
		for (int j = 0; j < i; j++)
		{
			if (formation_matrix.at<int>(i, j) == 1)
			{
				if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
				report_progress(progress_callback, progress_context,
					current_pair * 100 / std::max(1, pair_count), "Generating interferograms...");
				current_pair++;
				master_ix = i + 1; slave_ix = j + 1;
				B_temporal = temporal_baseline.at<double>(i, j);
				B_spatial = spatial_baseline.at<double>(i, j);

				ret = conversion.read_slc_from_h5(SLCH5Files[master_ix - 1].c_str(), master);
				if (return_check(ret, "read_slc_from_h5()", error_head)) return -1;
				if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
				ret = conversion.read_int_from_h5(SLCH5Files[master_ix - 1].c_str(), "offset_row", &offset_row);
				if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
				if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
				ret = conversion.read_int_from_h5(SLCH5Files[master_ix - 1].c_str(), "offset_col", &offset_col);
				if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
				if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
				ret = conversion.read_slc_from_h5(SLCH5Files[slave_ix - 1].c_str(), slave);
				if (return_check(ret, "read_slc_from_h5()", error_head)) return -1;
				if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
				if (master.type() != CV_32F) master.convertTo(master, CV_32F);
				if (slave.type() != CV_32F) slave.convertTo(slave, CV_32F);
				ret = util.Multilook(master, slave, multilook_rg, multilook_az, phase);
				if (return_check(ret, "Multilook()", error_head)) return -1;
				//滤波
				ret = filter.Goldstein_filter(phase, phase, alpha, 64, 8);
				if (return_check(ret, "Goldstein_filter()", error_head)) return -1;
				//计算相关系数
				ret = util.phase_coherence(phase, coherence);
				if (return_check(ret, "phase_coherence()", error_head)) return -1;
				if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
				snprintf(str, sizeof(str), "\\%d_%d.h5", i + 1, j + 1);
				h5file = path + str;
				if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
				ret = conversion.validate_distinct_h5_output(SLCH5Files[master_ix - 1].c_str(), h5file.c_str());
				if (return_check(ret, "validate_distinct_h5_output()", error_head)) return -1;
				ret = conversion.creat_new_h5(h5file.c_str());
				if (return_check(ret, "creat_new_h5()", error_head)) return -1;
				if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
				ret = conversion.write_array_to_h5(h5file.c_str(), "phase", phase);
				if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
				if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
				cv::Mat coherence_32f;
				coherence.convertTo(coherence_32f, CV_32F);
				ret = conversion.write_array_to_h5(h5file.c_str(), "coherence", coherence_32f);
				ret = conversion.write_int_to_h5(h5file.c_str(), "offset_row", offset_row);
				if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
				ret = conversion.write_int_to_h5(h5file.c_str(), "offset_col", offset_col);
				if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
				ret = conversion.write_int_to_h5(h5file.c_str(), "range_len", phase.cols);
				if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
				ret = conversion.write_int_to_h5(h5file.c_str(), "azimuth_len", phase.rows);
				if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
				ret = conversion.write_int_to_h5(h5file.c_str(), "multilook_rg", multilook_rg);
				if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
				ret = conversion.write_int_to_h5(h5file.c_str(), "multilook_az", multilook_az);
				if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
				ret = conversion.write_double_to_h5(h5file.c_str(), "B_temporal", B_temporal);
				if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
				ret = conversion.write_double_to_h5(h5file.c_str(), "B_spatial", B_spatial);
				if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
				ret = conversion.Copy_para_from_h5_2_h5(SLCH5Files[master_ix - 1].c_str(), h5file.c_str());
				if (return_check(ret, "Copy_para_from_h5_2_h5()", error_head)) return -1;
				if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
				ret = conversion.read_array_from_h5(SLCH5Files[master_ix - 1].c_str(), "mapped_lat", mapped_lat);
				if (ret == 0 && !b_mappedLatLon_written)
				{
					conversion.write_array_to_h5(h5file.c_str(), "mapped_lat", mapped_lat);
					conversion.read_array_from_h5(SLCH5Files[master_ix - 1].c_str(), "mapped_lon", mapped_lon);
					conversion.write_array_to_h5(h5file.c_str(), "mapped_lon", mapped_lon);
					b_mappedLatLon_written = true;
				}
				//是否保存为图片
				if (b_save_images)
				{
					if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
					sprintf(str, "\\%d_%d.jpg", i + 1, j + 1);
					h5file = path + str;
					ret = util.savephase(h5file.c_str(), "jet", phase);
					if (return_check(ret, "savephase()", error_head)) return -1;
				}
			}
		}
	}
	return 0;
}

int SBAS::compute_temporal_coherence(const Mat& estimated_phase_series, const Mat& phase_series, double* temporal_coherence)
{
	if (estimated_phase_series.size() != phase_series.size() ||
		estimated_phase_series.cols != 1 ||
		phase_series.type() != CV_64F ||
		phase_series.type() != estimated_phase_series.type() ||
		!temporal_coherence)
	{
		fprintf(stderr, "compute_temporal_coherence(): input check failed!\n");
		return -1;
	}
	int M = phase_series.rows;
	Mat temp = estimated_phase_series - phase_series;
	double real = 0.0, imag = 0.0;
	for (int i = 0; i < M; i++)
	{
		real += cos(temp.at<double>(i, 0));
		imag += sin(temp.at<double>(i, 0));
	}
	*temporal_coherence = sqrt(real * real + imag * imag) / (double)M;
	return 0;
}

int SBAS::adaptive_multilooking(
	vector<string>& coregis_slc_files,
	const char* ifgSavePath, 
	const Mat& formation_matrix, 
	const Mat& spatial_baseline, 
	const Mat& temporal_baseline,
	int blocksize_row, 
	int blocksize_col,
	Mat& out_mask,
	bool b_coh_est, 
	int homogeneous_test_wnd,
	double thresh_c1_to_c2, 
	bool b_normalize,
	bool b_save_images,
	IsCancelledCallback is_cancelled_callback,
	void* cancel_context,
	InSARProgressCallback progress_callback,
	void* progress_context
)
{
	if (coregis_slc_files.size() < 2 ||
		!ifgSavePath ||
		formation_matrix.empty() ||
		spatial_baseline.rows != formation_matrix.rows ||
		temporal_baseline.rows != formation_matrix.rows ||
		spatial_baseline.cols != formation_matrix.cols ||
		temporal_baseline.cols != formation_matrix.cols ||
		formation_matrix.type() != CV_32S ||
		temporal_baseline.type() != CV_64F ||
		spatial_baseline.type() != CV_64F ||
		blocksize_row < homogeneous_test_wnd ||
		blocksize_col < homogeneous_test_wnd ||
		thresh_c1_to_c2 < 0.0 ||
		thresh_c1_to_c2 > 1.0 ||
		homogeneous_test_wnd % 2 == 0)
	{
		fprintf(stderr, "adaptive_multilooking(): input check failed!\n");
		return -1;
	}

	int ret;
	int n_images = formation_matrix.rows;
	Utils util; FormatConversion conversion;
	std::string path(ifgSavePath), h5file;
	std::replace(path.begin(), path.end(), '/', '\\');
	ComplexMat master, slave;
	vector<string> h5file_list;
	char str[256];
	int master_ix, slave_ix, offset_row, offset_col, nr, nc, range_len, azimuth_len;
	int count = 0;//干涉图幅数
	double B_temporal, B_spatial;

	ret = conversion.read_int_from_h5(coregis_slc_files[0].c_str(), "range_len", &range_len);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
	ret = conversion.read_int_from_h5(coregis_slc_files[0].c_str(), "azimuth_len", &azimuth_len);
	if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
	if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
	Mat phase = Mat::zeros(azimuth_len, range_len, CV_64F);
	nr = azimuth_len; nc = range_len;
	Mat mask = Mat::zeros(azimuth_len, range_len, CV_32S); mask.copyTo(out_mask); mask.release();

	for (int i = 0; i < n_images; i++)
	{
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		for (int j = 0; j < i; j++)
		{
			if (formation_matrix.at<int>(i, j) == 1)
			{
				if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
				count++;
				master_ix = i + 1; slave_ix = j + 1;
				B_temporal = temporal_baseline.at<double>(i, j);
				B_spatial = spatial_baseline.at<double>(i, j);

				ret = conversion.read_int_from_h5(coregis_slc_files[master_ix - 1].c_str(), "offset_row", &offset_row);
				if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
				if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
				ret = conversion.read_int_from_h5(coregis_slc_files[master_ix - 1].c_str(), "offset_col", &offset_col);
				if (return_check(ret, "read_int_from_h5()", error_head)) return -1;
				if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;

				snprintf(str, sizeof(str), "\\%d_%d.h5", i + 1, j + 1);
				h5file = path + str;
				ret = conversion.validate_distinct_h5_output(coregis_slc_files[master_ix - 1].c_str(), h5file.c_str());
				if (return_check(ret, "validate_distinct_h5_output()", error_head)) return -1;
				ret = conversion.creat_new_h5(h5file.c_str());
				if (return_check(ret, "creat_new_h5()", error_head)) return -1;
				if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
				h5file_list.push_back(h5file);
				//预先填充相位和相关系数
				ret = conversion.write_array_to_h5(h5file.c_str(), "phase", phase);
				if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
				cv::Mat phase_32f;
				phase.convertTo(phase_32f, CV_32F);
				ret = conversion.write_array_to_h5(h5file.c_str(), "coherence", phase_32f);
				if (return_check(ret, "write_array_to_h5()", error_head)) return -1;
				ret = conversion.write_int_to_h5(h5file.c_str(), "offset_row", offset_row);
				if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
				ret = conversion.write_int_to_h5(h5file.c_str(), "offset_col", offset_col);
				if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
				ret = conversion.write_int_to_h5(h5file.c_str(), "range_len", phase.cols);
				if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
				ret = conversion.write_int_to_h5(h5file.c_str(), "azimuth_len", phase.rows);
				if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
				ret = conversion.write_int_to_h5(h5file.c_str(), "multilook_rg", 1);
				if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
				ret = conversion.write_int_to_h5(h5file.c_str(), "multilook_az", 1);
				if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
				ret = conversion.write_double_to_h5(h5file.c_str(), "B_temporal", B_temporal);
				if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
				ret = conversion.write_double_to_h5(h5file.c_str(), "B_spatial", B_spatial);
				if (return_check(ret, "write_int_to_h5()", error_head)) return -1;
				ret = conversion.Copy_para_from_h5_2_h5(coregis_slc_files[master_ix - 1].c_str(), h5file.c_str());
				if (return_check(ret, "Copy_para_from_h5_2_h5()", error_head)) return -1;
			}
		}
	}





	int homotest_radius = (homogeneous_test_wnd - 1) / 2;

	//分块读取、计算和储存
	std::atomic<int> completed_rows(0);
	std::atomic<bool> cancel_flag(false);
	int step = std::max(1, nr / 100);

	int left, right, top, bottom, block_num_row, block_num_col, left_pad, right_pad, top_pad, bottom_pad;
	vector<ComplexMat> slc_series, slc_series_filter;
	vector<Mat> coherence_series; coherence_series.resize(count);
	ComplexMat slc, slc2, temp;
	Mat flat_phase, ph, zeromat;
	if (nr % blocksize_row == 0) block_num_row = nr / blocksize_row;
	else block_num_row = int(floor((double)nr / (double)blocksize_row)) + 1;
	if (nc % blocksize_col == 0) block_num_col = nc / blocksize_col;
	else block_num_col = int(floor((double)nc / (double)blocksize_col)) + 1;
	for (int i = 0; i < block_num_row; i++)
	{
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		if (cancel_flag) {
			break;
		}
		for (int j = 0; j < block_num_col; j++)
		{
			if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
			if (cancel_flag) {
				break;
			}
			top = i * blocksize_row;
			top_pad = top - homotest_radius; top_pad = top_pad < 0 ? 0 : top_pad;
			bottom = top + blocksize_row; bottom = bottom > nr ? nr : bottom;
			bottom_pad = bottom + homotest_radius; bottom_pad = bottom_pad > nr ? nr : bottom_pad;
			left = j * blocksize_col;
			left_pad = left - homotest_radius; left_pad = left_pad < 0 ? 0 : left_pad;
			right = left + blocksize_col; right = right > nc ? nc : right;
			right_pad = right + homotest_radius; right_pad = right_pad > nc ? nc : right_pad;

			//读取数据
			for (int k = 0; k < n_images; k++)
			{
				if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
				ret = conversion.read_subarray_from_h5(coregis_slc_files[k].c_str(), "s_re",
					top_pad, left_pad, bottom_pad - top_pad, right_pad - left_pad, slc.re);
				if (return_check(ret, "read_subarray_from_h5()", error_head)) return -1;
				if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
				ret = conversion.read_subarray_from_h5(coregis_slc_files[k].c_str(), "s_im",
					top_pad, left_pad, bottom_pad - top_pad, right_pad - left_pad, slc.im);
				if (return_check(ret, "read_subarray_from_h5()", error_head)) return -1;
				if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
				if (slc.type() != CV_64F) slc.convertTo(slc, CV_64F);
				slc_series.push_back(slc);
				slc_series_filter.push_back(slc);
			}

			//填充相关系数
			if (b_coh_est)
			{
				zeromat = Mat::zeros(slc.GetRows(), slc.GetCols(), CV_64F);
				for (int mm = 0; mm < count; mm++)
				{
					zeromat.copyTo(coherence_series[mm]);
				}
			}
			//计算
#pragma omp parallel for schedule(guided)
			for (int ii = (top - top_pad); ii < (bottom - top_pad); ii++)
			{
				if (cancel_flag.load(std::memory_order_relaxed) ||
					is_cancelled(is_cancelled_callback, cancel_context)) {
					cancel_flag.store(true, std::memory_order_relaxed);
					continue;
				}
				ComplexMat coherence_matrix, eigenvector; Mat eigenvalue; int ret1, count_parallel;
				for (int jj = (left - left_pad); jj < (right - left_pad); jj++)
				{
					if ((jj & 0x3f) == 0 &&
						is_cancelled(is_cancelled_callback, cancel_context)) {
						cancel_flag.store(true, std::memory_order_relaxed);
						break;
					}
					ret1 = util.coherence_matrix_estimation(slc_series, coherence_matrix, homogeneous_test_wnd, homogeneous_test_wnd, ii, jj);
					if (ret1 == 0)
					{
						ret1 = util.HermitianEVD(coherence_matrix, eigenvalue, eigenvector);
						if (!eigenvalue.empty() && ret1 == 0)
						{
							if (eigenvalue.at<double>(1, 0) / (eigenvalue.at<double>(0, 0) + 1e-10) < thresh_c1_to_c2)
							{
								out_mask.at<int>(ii + top_pad, jj + left_pad) = 1;
								for (int kk = 0; kk < n_images; kk++)
								{
									slc_series_filter[kk].re.at<double>(ii, jj) = eigenvector.re.at<double>(kk, 0);
									slc_series_filter[kk].im.at<double>(ii, jj) = eigenvector.im.at<double>(kk, 0);
								}
								if (b_coh_est)
								{
									count_parallel = 0;
									for (int iii = 0; iii < n_images; iii++)
									{
										for (int jjj = 0; jjj < iii; jjj++)
										{
											if (formation_matrix.at<int>(iii, jjj) == 1)
											{
												double real_val = coherence_matrix.re.at<double>(iii, jjj);
												double imag_val = coherence_matrix.im.at<double>(iii, jjj);
												coherence_series[count_parallel].at<double>(ii, jj) = sqrt(real_val * real_val + imag_val * imag_val);
												count_parallel++;
											}
										}
									}
								}
							}
						}
					}


				}

				int current_completed = ++completed_rows;
				if (current_completed % step == 0)
				{
					int progress = current_completed * 100 / nr;
					report_progress(progress_callback, progress_context,
						progress, "Adaptive multilooking...");
				}
			}
			if (cancel_flag.load(std::memory_order_relaxed) ||
				is_cancelled(is_cancelled_callback, cancel_context)) return -2;

			//储存
			int kk = 0;
			for (int iii = 0; iii < n_images; iii++)
			{
				for (int jjj = 0; jjj < iii; jjj++)
				{
					if (formation_matrix.at<int>(iii, jjj) == 1)
					{
						if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
						if (b_coh_est && !coherence_series.empty() && !coherence_series[kk].empty())
						{
							cv::Mat coh_sub = coherence_series[kk](cv::Range(top - top_pad, bottom - top_pad), cv::Range(left - left_pad, right - left_pad));
							cv::Mat coh_32f;
							coh_sub.convertTo(coh_32f, CV_32F);
							ret = conversion.write_subarray_to_h5(h5file_list[kk].c_str(), "coherence", coh_32f, top, left, bottom - top, right - left);
							if (return_check(ret, "write_subarray_to_h5()", error_head)) return -1;
							if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
						}
						ret = util.multilook(slc_series_filter[iii], slc_series_filter[jjj], 1, 1, phase);
						if (return_check(ret, "multilook()", error_head)) return -1;
						phase(cv::Range(top - top_pad, bottom - top_pad), cv::Range(left - left_pad, right - left_pad)).copyTo(ph);
						ret = conversion.write_subarray_to_h5(h5file_list[kk].c_str(), "phase", ph, top, left, bottom - top, right - left);
						if (return_check(ret, "write_subarray_to_h5()", error_head)) return -1;
						if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
						kk++;
					}
				}
			}
			slc_series.clear();
			slc_series_filter.clear();
			fprintf(stdout, "估计相位进度：%.1lf\n", double((i + 1) * block_num_col + j + 1) / double((block_num_col) * (block_num_row)));
		}
	}

	if (cancel_flag)
	{
		return -2;
	}


	//是否保存为图片
	if (b_save_images)
	{
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		for (int iii = 0; iii < n_images; iii++)
		{
			for (int jjj = 0; jjj < iii; jjj++)
			{
				if (formation_matrix.at<int>(iii, jjj) == 1)
				{
					if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
					//相位
					sprintf(str, "\\%d_%d.h5", iii + 1, jjj + 1);
					h5file = path + str;
					ret = conversion.read_array_from_h5(h5file.c_str(), "phase", phase);
					if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
					sprintf(str, "\\%d_%d.jpg", iii + 1, jjj + 1);
					h5file = path + str;
					ret = util.savephase(h5file.c_str(), "jet", phase);
					if (return_check(ret, "savephase()", error_head)) return -1;
					//相关系数
					ret = conversion.read_array_from_h5(h5file.c_str(), "coherence", phase);
					if (return_check(ret, "read_array_from_h5()", error_head)) return -1;
					if (phase.type() != CV_64F) phase.convertTo(phase, CV_64F);
					sprintf(str, "\\%d_%d_coh.jpg", iii + 1, jjj + 1);
					h5file = path + str;
					ret = util.savephase(h5file.c_str(), "jet", phase);
					if (return_check(ret, "savephase()", error_head)) return -1;
				}
			}
		}
		
	}

	return 0;
}

int SBAS::refinement_and_reflattening(
	Mat& unwrapped_phase, const Mat& mask, const Mat& coherence, double coh_thresh,
	IsCancelledCallback is_cancelled_callback, void* cancel_context,
	InSARProgressCallback progress_callback, void* progress_context)
{
	if (unwrapped_phase.size() != mask.size() ||
		unwrapped_phase.size() != coherence.size() ||
		unwrapped_phase.empty() ||
		unwrapped_phase.type() != CV_64F ||
		mask.type() != CV_32S ||
		(coherence.type() != CV_64F && coherence.type() != CV_32F)
		)
	{
		fprintf(stderr, "refinement_and_reflattening(): input check failed!\n");
		return -1;
	}
	if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
	cv::Mat coh_64f = coherence;
	if (coh_64f.type() != CV_64F) coh_64f.convertTo(coh_64f, CV_64F);
	int mask_count = cv::countNonZero(mask);
	Mat A(mask_count, 3, CV_64F), b(mask_count, 1, CV_64F);
	A = 1.0;
	int count = 0, rows = mask.rows, cols = mask.cols;
	for (int i = 0; i < rows; i++)
	{
		if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;
		for (int j = 0; j < cols; j++)
		{
			if ((j & 0x3ff) == 0 &&
				is_cancelled(is_cancelled_callback, cancel_context)) return -2;
			if (mask.at<int>(i, j) == 1 && coh_64f.at<double>(i, j) > coh_thresh)
			{
				A.at<double>(count, 1) = (double)i;
				A.at<double>(count, 2) = (double)j;
				b.at<double>(count, 0) = unwrapped_phase.at<double>(i, j);
				count++;
			}
		}
		if (i % std::max(1, rows / 100) == 0) {
			report_progress(progress_callback, progress_context,
				i * 50 / std::max(1, rows), "Fitting reflattening plane...");
		}
	}
	
	if (count < 3)
	{
		return 0;
	}
	A(cv::Range(0, count), cv::Range(0, 3)).copyTo(A);
	b(cv::Range(0, count), cv::Range(0, 1)).copyTo(b);

	Mat A_t, x;
	cv::transpose(A, A_t);
	b = A_t * b;
	A = A_t * A;
	if (!cv::solve(A, b, x, cv::DECOMP_NORMAL))
	{
		return 0;
	}
	if (is_cancelled(is_cancelled_callback, cancel_context)) return -2;

	//找到参考点坐标
	//int ref_i, ref_j;
	//count = 0;
	//for (int i = 0; i < rows; i++)
	//{
	//	for (int j = 0; j < cols; j++)
	//	{
	//		if (mask.at<int>(i, j) == 1)
	//		{
	//			count++;
	//			if (count == reference) { ref_i = i; ref_j = j; }
	//		}
	//	}
	//}
	//double phase_ref = unwrapped_phase.at<double>(ref_i, ref_j);
	double coef_intercept = x.at<double>(0, 0);
	double coef_row = x.at<double>(1, 0);
	double coef_col = x.at<double>(2, 0);
	Mat refined_phase = unwrapped_phase.clone();
	std::atomic<bool> cancel_flag(false);
	#pragma omp parallel for schedule(guided)
	for (int i = 0; i < rows; i++)
	{
		if (cancel_flag.load(std::memory_order_relaxed) ||
			is_cancelled(is_cancelled_callback, cancel_context)) {
			cancel_flag.store(true, std::memory_order_relaxed);
			continue;
		}
		for (int j = 0; j < cols; j++)
		{
			if ((j & 0x3ff) == 0 &&
				is_cancelled(is_cancelled_callback, cancel_context)) {
				cancel_flag.store(true, std::memory_order_relaxed);
				break;
			}
			refined_phase.at<double>(i, j) = unwrapped_phase.at<double>(i, j) -
				(coef_intercept + coef_row * double(i) + coef_col * double(j));
		}
	}
	if (cancel_flag.load(std::memory_order_relaxed) ||
		is_cancelled(is_cancelled_callback, cancel_context)) return -2;
	refined_phase.copyTo(unwrapped_phase);
	//unwrapped_phase = unwrapped_phase - (unwrapped_phase.at<double>(ref_i, ref_j) - phase_ref);

	return 0;
}

int SBAS::write_high_coherence_node(const Mat& mask, const char* filename)
{
	if (filename == NULL ||
		mask.rows < 2 ||
		mask.cols < 2 ||
		mask.channels() != 1 ||
		mask.type() != CV_32S
		)
	{
		fprintf(stderr, "write_high_coherence_node(): input check failed!\n\n");
		return -1;
	}
	FILE* fp = NULL;
	fp = fopen(filename, "wt");
	if (fp == NULL)
	{
		fprintf(stderr, "write_high_coherence_node(): can't open %s\n", filename);
		return -1;
	}
	int nonzero = cv::countNonZero(mask);
	if (nonzero <= 0)
	{
		fprintf(stderr, "write_high_coherence_node(): no node exist!\n");
		if (fp)
		{
			fclose(fp);
			fp = NULL;
		}
		return -1;
	}
	int dim, attr1, attr2, count;
	dim = 2;
	attr1 = 0;
	attr2 = 0;
	fprintf(fp, "%d %d %d %d\n", nonzero, dim, attr1, attr2);
	int rows = mask.rows;
	int cols = mask.cols;
	count = 1;
	double x, y;
	for (int i = 0; i < rows; i++)
	{
		for (int j = 0; j < cols; j++)
		{
			if (mask.at<int>(i, j) > 0)
			{
				x = double(rows - i);
				y = double(j + 1);
				fprintf(fp, "%d %lf %lf\n", count, x, y);
				count++;
			}
		}
	}
	if (fp)
	{
		fclose(fp);
		fp = NULL;
	}
	return 0;
}

int SBAS::set_high_coherence_node_coordinate(
	const Mat& mask,
	vector<SBAS_node>& nodes
)
{
	if (mask.empty() ||
		mask.type() != CV_32S ||
		static_cast<int>(nodes.size()) < 3
		)
	{
		fprintf(stderr, "set_high_coherence_node_coordinate(): input check failed!\n");
		return -1;
	}
	int rows = mask.rows;
	int cols = mask.cols;
	int nonzero = cv::countNonZero(mask);
	int num_nodes = static_cast<int>(nodes.size());
	if (num_nodes != nonzero)
	{
		fprintf(stderr, "set_high_coherence_node_coordinate(): nodes and mask mismatch!\n");
		return -1;
	}
	int count = 0;
	for (int i = 0; i < rows; i++)
	{
		for (int j = 0; j < cols; j++)
		{
			if (mask.at<int>(i, j) > 0)
			{
				nodes[count].x = j + 1;
				nodes[count].y = rows - i;
				count++;
			}
		}
	}
	return 0;
}

int SBAS::set_high_coherence_node_phase(
	const Mat& mask, 
	vector<SBAS_node>& nodes, 
	vector<SBAS_edge>& edges,
	const Mat& phase
)
{
	if (mask.empty() ||
		mask.type() != CV_32S ||
		nodes.size() < 3 || 
		phase.rows != mask.rows ||
		phase.cols != mask.cols ||
		phase.type() != CV_64F ||
		edges.size() < 3
		)
	{
		fprintf(stderr, "set_high_coherence_node_phase(): input check failed!\n");
		return -1;
	}
	int rows = mask.rows;
	int cols = mask.cols;
	int nonzero = cv::countNonZero(mask);
	int num_nodes = static_cast<int>(nodes.size());
	int num_edges = static_cast<int>(edges.size());
	if (num_nodes != nonzero)
	{
		fprintf(stderr, "set_high_coherence_node_phase(): nodes and mask mismatch!\n");
		return -1;
	}
	int count = 0;
	for (int i = 0; i < rows; i++)
	{
		for (int j = 0; j < cols; j++)
		{
			if (mask.at<int>(i, j) > 0)
			{
				nodes[count].phase = phase.at<double>(i, j);
				count++;
			}
		}
	}
#pragma omp parallel for schedule(guided)
	for (int i = 0; i < num_edges; i++)
	{
		int end1, end2;
		double gradient;
		end1 = edges[i].end1; end2 = edges[i].end2;
		if (end1 > end2)
		{
			gradient = nodes[end1 - 1].phase - nodes[end2 - 1].phase;
			gradient = atan2(sin(gradient), cos(gradient));
			edges[i].phase_gradient = gradient;
		}
		else
		{
			gradient = nodes[end2 - 1].phase - nodes[end1 - 1].phase;
			gradient = atan2(sin(gradient), cos(gradient));
			edges[i].phase_gradient = gradient;
		}
	}
	return 0;
}
