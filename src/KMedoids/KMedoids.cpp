#include "KMedoids.h"


/*
 * @brief The bool tag whether the data set is from PBF simulation
 */
extern bool isPBF;


/*
 * @brief The k-medoids class constructor with parameter and data set input
 *
 * @param[in] pm The Parameter class object for the user input control
 * @param[in] data The matrix coordinates of the streamlines
 * @param[in] numOfClusters The number of clusters as input
 */
KMedoids::KMedoids(const Parameter& pm, 
				   const Eigen::MatrixXf& data,
				   const int& numOfClusters)
		 :initialStates(pm.initialization), isSample(pm.isSample),
		  data(data), numOfClusters(numOfClusters)
{

}
	

/*
 * @brief The destructor
 */
KMedoids::~KMedoids()
{

}


/*
 * @brief Perform the k-medoids clustering technique and calculate the clustering evaluation metrics
 *
 * @param[out] fline The feature line extraction result (centroid, closest, furthest candidates)
 * @param[in] normOption The norm option
 * @param[out] sil Silhouette The Silhouette class object to record clustering evaluation metrics
 * @param[out] tr The TimeRecorder class object
 */
void KMedoids::getMedoids(FeatureLine& fline,
						  const int& normOption,
						  Silhouette& sil,
						  TimeRecorder& tr) const
{
	MetricPreparation object(data.rows(), data.cols());
	object.preprocessing(data, data.rows(), data.cols(), normOption);

	MatrixXf clusterCenter;
	getInitCenter(clusterCenter, object, normOption);

	float moving=1000, tempMoving,/* dist, tempDist, */before;
	int *storage = new int[numOfClusters]; // used to store number inside each cluster
	MatrixXf centerTemp;
	int tag = 0;
	std::vector< std::vector<int> > neighborVec(numOfClusters, 
												std::vector<int>());

/* perform K-means with different metrics */
	std::cout << "K-medoids start!" << std::endl;
	const int& Row = data.rows();
	const int& Column = data.cols();	
	struct timeval start, end;
	gettimeofday(&start, NULL);
	std::vector<int> recorder(Row); //use to record which cluster the row belongs to

	do
	{
	/* reset storage number and weighted mean inside each cluster*/
		before=moving;
		memset(storage,0,sizeof(int)*numOfClusters);
		centerTemp = clusterCenter;

	/* clear streamline indices for each cluster */
	#pragma omp parallel for schedule(static) num_threads(8)
		for (int i = 0; i < numOfClusters; ++i)
		{
			neighborVec[i].clear();
		}

	#pragma omp parallel num_threads(8)
		{
		#pragma omp for nowait
			for (int i = 0; i < Row; ++i)
			{
				int clusTemp;
				float dist = FLT_MAX;
				float tempDist;
				for (int j = 0; j < numOfClusters; ++j)
				{
					tempDist = getDisimilarity(clusterCenter.row(j),
								data,i,normOption,object);
					if(tempDist<dist)
					{
						dist = tempDist;
						clusTemp = j;
					}
				}
				recorder[i] = clusTemp;

			#pragma omp critical
				{
					storage[clusTemp]++;
					neighborVec[clusTemp].push_back(i);
				}
			}
		}

		computeMedoids(centerTemp, neighborVec, normOption, object);

		moving = FLT_MIN;

	/* measure how much the current center moves from original center */	
	#pragma omp parallel for reduction(max:moving) num_threads(8)
		for (int i = 0; i < numOfClusters; ++i)
		{
			if(storage[i]>0)
			{
				tempMoving = (centerTemp.row(i)-clusterCenter.row(i)).norm();
				clusterCenter.row(i) = centerTemp.row(i);
				if(moving<tempMoving)
					moving = tempMoving;
			}
		}
		std::cout << "K-means iteration " << ++tag << " completed, and moving is " << moving 
				  << "!" << std::endl;
	}while(abs(moving-before)/before >= 1.0e-2 && tag < 20/* && moving > 5.0*/);
	
	double delta;

	tr.eventList.push_back("For norm ");
	tr.timeList.push_back(to_string(normOption)+"\n");

	std::multimap<int,int> groupMap;
	float entropy = 0.0;
	float probability;
	vector<int> increasingOrder(numOfClusters);
	for (int i = 0; i < numOfClusters; ++i)
	{
		groupMap.insert(std::pair<int,int>(storage[i],i));
		if(storage[i]>0)
		{
			probability = float(storage[i])/float(Row);
			entropy += probability*log2f(probability);
		}
	}

	int groupNo = 0;
	for (std::multimap<int,int>::iterator it = groupMap.begin(); it != groupMap.end(); ++it)
	{
		if(it->first>0)
		{
			increasingOrder[it->second] = (groupNo++);
		}
	}

	entropy = -entropy/log2f(groupNo);
	/* finish tagging for each group */

	/* record labeling information */
	// IOHandler::generateGroups(neighborVec);


	// set cluster group number and size number 
#pragma omp parallel for schedule(static) num_threads(8)
	for (int i = 0; i < Row; ++i)
	{
		fline.group[i] = increasingOrder[recorder[i]];
		fline.totalNum[i] = storage[recorder[i]];
	}

	float shortest, toCenter, farDist;
	int shortestIndex = 0, tempIndex = 0, furthestIndex = 0;
	std::vector<int> neighborTemp;

	/* choose cloest and furthest streamlines to centroid streamlines */
	for (int i = 0; i < numOfClusters; ++i)
	{
		if(storage[i]>0)
		{

			neighborTemp = neighborVec[i];
			shortest = FLT_MAX;
			farDist = FLT_MIN;

			for (int j = 0; j < storage[i]; ++j)
			{
				// j-th internal streamlines 
				tempIndex = neighborTemp[j];
				toCenter = getDisimilarity(clusterCenter.row(i),data,
							tempIndex,normOption,object);
				if(!isSample)
				{
					if(toCenter<shortest)
					{
						shortest = toCenter;
						shortestIndex = tempIndex;
					}
				}
				/* update the farthest index to centroid */
				if(toCenter>farDist)
				{
					farDist = toCenter;
					furthestIndex = tempIndex;
				}
			}
			if(!isSample)
				fline.closest.push_back(ExtractedLine(shortestIndex,increasingOrder[i]));
			fline.furthest.push_back(ExtractedLine(furthestIndex,increasingOrder[i]));
		}
	}

	std::vector<float> closeSubset;
	/* based on known cluster centroid, save them as vector for output */
	for (int i = 0; i < numOfClusters; ++i)
	{
		if(storage[i]>0)
		{
			for (int j = 0; j < Column; ++j)
			{
				closeSubset.push_back(clusterCenter(i,j));
			}
			fline.centerMass.push_back(MeanLine(closeSubset,increasingOrder[i]));
			closeSubset.clear();
		}
	}
	delete[] storage;
	std::cout << "Has taken closest and furthest out!" << std::endl;


/* Silhouette computation started */

	std::cout << "The finalized cluster size is: " << groupNo << std::endl;
	if(groupNo<=1)
		return;

	/* if the dataset is not PBF, then should record distance matrix for Gamma matrix compution */
	if(!isPBF)
	{
		deleteDistanceMatrix(data.rows());

		std::ifstream distFile(("../dataset/"+to_string(normOption)).c_str(), ios::in);
		if(distFile.fail())
		{
			distFile.close();
			getDistanceMatrix(data, normOption, object);
			std::ofstream distFileOut(("../dataset/"+to_string(normOption)).c_str(), ios::out);
			for(int i=0;i<data.rows();++i)
			{
				for(int j=0;j<data.rows();++j)
				{
					distFileOut << distanceMatrix[i][j] << " ";
				}
				distFileOut << std::endl;
			}
			distFileOut.close();
		}
		else
		{
			std::cout << "read distance matrix..." << std::endl;

			distanceMatrix = new float*[data.rows()];
		#pragma omp parallel for schedule(static) num_threads(8)
			for (int i = 0; i < data.rows(); ++i)
			{
				distanceMatrix[i] = new float[data.rows()];
			}
			int i=0, j;
			string line;
			stringstream ss;
			while(getline(distFile, line))
			{
				j=0;
				ss.str(line);
				while(ss>>line)
				{
					if(i==j)
						distanceMatrix[i][j]=0;
					else
						distanceMatrix[i][j] = std::atof(line.c_str());
					++j;
				}
				++i;
				ss.str("");
				ss.clear();
			}
			distFile.close();
		}
	}

	tr.eventList.push_back("Final cluster number is : ");
	tr.timeList.push_back(to_string(groupNo));

	ValidityMeasurement vm;
	vm.computeValue(normOption, data, fline.group, object, isPBF);

	tr.eventList.push_back("Kmedoids Validity measure is: ");
	stringstream fc_ss;
	fc_ss << vm.f_c;
	tr.timeList.push_back(fc_ss.str());

	//groupNo record group numbers */
	gettimeofday(&start, NULL);

	sil.computeValue(normOption,data,Row,Column,fline.group,object,groupNo,isPBF);
	std::cout << "Silhouette computation completed!" << std::endl;

	gettimeofday(&end, NULL);
	delta = ((end.tv_sec  - start.tv_sec) * 1000000u + end.tv_usec - start.tv_usec) / 1.e6;

	tr.eventList.push_back("Evaluation analysis would take: ");
	tr.timeList.push_back(to_string(delta)+"s");

	/* write value of the silhouette class */
	IOHandler::writeReadme(entropy, sil, "For norm "+to_string(normOption));
}


/*
 * @brief Get the initial centroid by k-medoids initialization
 *
 * @param[out] initialCenter The initialized center coordinates
 * @param[in] object The MetricPreparation object for calculating the distance values
 * @param[in] normOption The norm option
 */
void KMedoids::getInitCenter(MatrixXf& initialCenter,
							 const MetricPreparation& object,
							 const int& normOption) const
{
	switch(initialStates)
	{
	case 1:
		Initialization::generateRandomPos(initialCenter, data.cols(), data, numOfClusters);
		break;

	default:
	case 2:
		Initialization::generateFromSamples(initialCenter, data.cols(), data, numOfClusters);
		break;

	case 3:
		Initialization::generateFarSamples(initialCenter, data.cols(), data, numOfClusters, normOption, object);
		break;
	}
	std::cout << "Initialization completed!" << std::endl;
}


/*
 * @brief Compute the medoids by either calculating the median or iteration
 *
 * @param[out] centerTemp The medoid coordinates to be updated
 * @param[in] neighborVec The neighboring candidates belonging to a cluster
 * @param[in] normOption The norm option
 * @param[in] object The MetricPreparation object for distance calculation
 */
void KMedoids::computeMedoids(MatrixXf& centerTemp, 
							  const vector<vector<int> >& neighborVec, 
							  const int& normOption, 
							  const MetricPreparation& object) const
{
	centerTemp = MatrixXf(numOfClusters,data.cols());
	if(isSample)//centroid is from samples with minimal L1 summation
				//use Voronoi iteration https://en.wikipedia.org/wiki/K-medoids
	{
	#pragma omp parallel for schedule(static) num_threads(8)
		for(int i=0;i<neighborVec.size();++i)
		{
			const vector<int>& clusMember = neighborVec[i];
			const int& clusSize = clusMember.size();
			MatrixXf mutualDist = MatrixXf::Zero(clusSize, clusSize);
			/*mutualDist to store mutual distance among lines of each cluster */
			for(int j=0;j<clusSize;++j)
			{
				for(int k=j+1;k<clusSize;++k)
				{
					mutualDist(j,k) = getDisimilarity(data,clusMember[j],
						clusMember[k],normOption,object);
					mutualDist(k,j) = mutualDist(j,k);
				}
			}

			float minL1_norm = FLT_MAX, rowSummation;
			int index = -1;
			for(int j=0;j<clusSize;++j)
			{
				rowSummation = mutualDist.row(j).sum();
				if(rowSummation<minL1_norm)
				{
					minL1_norm = rowSummation;
					index = j;
				}
			}
			centerTemp.row(i)=data.row(clusMember[index]);
		}
	}

	else//use Weiszfeld's algorithm to get geometric median
		//reference at https://en.wikipedia.org/wiki/Geometric_median
	{
		MatrixXf originCenter = centerTemp;
	#pragma omp parallel for schedule(static) num_threads(8)
		for(int i=0;i<numOfClusters;++i)
		{
			const vector<int>& clusMember = neighborVec[i];
			const int& clusSize = clusMember.size();
			float distToCenter, distInverse, percentage = 1.0;
			int tag = 0;
			while(tag<=10&&percentage>=0.02)
			{
				VectorXf numerator = VectorXf::Zero(data.cols());
				VectorXf previous = centerTemp.row(i);
				float denominator = 0;
				for(int j=0;j<clusSize;++j)
				{
					distToCenter = getDisimilarity(centerTemp.row(i),
							data,clusMember[j],normOption,object);
					distInverse = (distToCenter>1.0e-8)?1.0/distToCenter:1.0e8;
					numerator += data.row(clusMember[j])*distInverse;
					denominator += distInverse;
				}
				centerTemp.row(i) = numerator/denominator;
				percentage = (centerTemp.row(i)-previous).norm()/previous.norm();
				tag++;
			}
		}
	}
}

