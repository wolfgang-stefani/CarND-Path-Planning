#include <uWS/uWS.h>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "helpers.h"
#include "json.hpp"
#include "spline.h"

// for convenience
using nlohmann::json;
using std::string;
using std::vector;

class Vehicle{
public:
    int lane, s, future_s, car_id;
    float speed;
    vector<int> cal_prev_size;
    // F: front RB: right back RF: right front LB: left back LF: left front
    string state;
    
    void cal_mag_v(double v_x, double v_y, double prev_size){
         speed = sqrt(v_x*v_x + v_y*v_y);
        // Prediction the future location
        future_s = s + (double)prev_size * 0.02 * speed;
    }
};


class List_Vehicle{
public:
    vector<Vehicle> right_front;
    vector<Vehicle> right_back;
    vector<Vehicle> left_front;
    vector<Vehicle> left_back;
    Vehicle front;
    
    double ego_future_s,ego_s,ego_speed;
    double detect_range = 30;
    double follow_speed = 30.0;
   
    
    // Append new readings to the previouse. Handles if the new car appears in the readings
    void append(vector<Vehicle> &object, Vehicle item){
        bool found = false;
        // Initialize object for the first time
        if (object.size() == 0 ){
            object.push_back(item);
        }
        // If there are items inside object, search for the same car id
        else{
            for (int i = 0; i < object.size(); ++i) {
                // Same id found, update the infor base on the new item
                if (object[i].car_id == item.car_id) {
                    object[i].lane = item.lane;
                    object[i].cal_prev_size.push_back(abs(item.s-object[i].s)/(0.02 * item.speed));
                    object[i].s = item.s;
                    object[i].speed = item.speed;
                    object[i].future_s = item.future_s;
                    found = true;
                    // Jump out
                    break;
                }
            }
            // If not found, meaning new car appear in the reading
            if (!found) {
                object.push_back(item);
            }
        }
    }
    
    // Find if there is sensor reading missing from the previous result. This handles missing sensor reading when the sensored car is too close
    void trace_back (vector<Vehicle> &prev_reading, vector<Vehicle> now_reading, int prev_size){
        bool found = false;
        for (int i = 0; i < prev_reading.size(); ++i) {
            for (int j = 0; j < now_reading.size(); ++j) {
                if (prev_reading[i].car_id == now_reading[j].car_id){
                    // Found and exit
                    found = true;
                    break;
                }
            }
            if (!found){
                // Check if the previous is moving outside the range or too close
                // check  it is within the range now and in the future
                if ((abs(prev_reading[i].s - ego_s) < detect_range) || (abs(prev_reading[i].future_s - ego_future_s) < detect_range) ){
                    float averge_diff_size;
                    int size = prev_reading[i].cal_prev_size.size();
                    auto cal_prev_size = prev_reading[i].cal_prev_size;
                    if (size>0){
                       averge_diff_size = std::accumulate(cal_prev_size.begin(),cal_prev_size.end(), 0.0) / size;
                    }
                    //catch if the prev_s is empty
                    else averge_diff_size = (float)prev_size;
                    
                    // Update prediction according to previous reading
                    
                    int predict_s = prev_reading[i].s + (averge_diff_size * 0.02 * prev_reading[i].speed);
                    // assume it is still in the same lane
                    prev_reading[i].s = predict_s;
                    prev_reading[i].future_s = predict_s + (double)prev_size * 0.02 * prev_reading[i].speed;
                    
                }
                // if it is outside the range, delete this
                else{
                    prev_reading.erase(prev_reading.begin()+i);
                }
            }
            else found = false;
        }
        return;
    }
    
    void cal_future_ego_s (double ref_val, double car_s, double prev_size){
        ego_speed = ref_val/2.24; // Convert mph to m/s
        ego_s = car_s;
        ego_future_s = car_s + (double)prev_size * 0.02 * ego_speed;
    }
    
    
    Vehicle find_lowest_index(vector<Vehicle> sensor_reading){
        int temp_s = sensor_reading[0].s;
        int new_index = 0;
        for (int j = 0;j < sensor_reading.size();++j){
            if (sensor_reading[j].s <temp_s){
                new_index = j;
                temp_s =sensor_reading[j].s;
            }
        }
        return sensor_reading[new_index];
    }
    
    Vehicle get_closest_vehicle (vector<Vehicle> input_vehicle){
        if (input_vehicle.size() > 1){
            std::cout<<std::endl<<"Multiple Reading detected "<< input_vehicle[0].state<<std::endl;
            return find_lowest_index(input_vehicle);
        }
        else{
            return input_vehicle[0];
        }
    }

};



float follow_cost (Vehicle sensor_reading, double self_speed,double car_s,bool front_cost,bool future_s = false, double detect_range = 30.0 ){
    float target_car_speed = sensor_reading.speed;
    float norm_speed = (target_car_speed - self_speed)/self_speed;
    float dist_cost;
    float speed_cost;
    if (future_s){
        // if uses future value of s, car_s need to be also future value
        dist_cost = 1 - abs(sensor_reading.future_s - car_s)/detect_range;
    }
    
    else{
     dist_cost = 1 - abs(sensor_reading.s - car_s)/detect_range;
    }
    
    if (front_cost) {
        speed_cost = pow(2,-norm_speed) - norm_speed;
    }
    else{
        speed_cost = pow(2,norm_speed) + norm_speed;
    }
    return (speed_cost + dist_cost)/2;

}

double one_side_cost (vector<Vehicle> front, vector<Vehicle> back, double self_speed, double ego_s, double detect_range,bool future_s_flag){
    List_Vehicle sample;
    double front_cost,back_cost;
    if (front.size()>0){
        front_cost = follow_cost(sample.get_closest_vehicle(front), self_speed, ego_s, true, future_s_flag, detect_range);
    }
    else front_cost = 0.0;
    
    if (back.size()>0){
        back_cost = follow_cost(sample.get_closest_vehicle(back), self_speed, ego_s, false, future_s_flag, detect_range);
    }
    else back_cost = 0.0;
    // Calculate total Right cost
    std::cout<<"front cost: "<< front_cost<< std::endl;
    std::cout<<"back cost: "<< back_cost<< std::endl;
    double cost;
    // Calculate total Right cost
    if (front_cost != 0.0 && back_cost != 0.0 ) {
        cost = (front_cost + back_cost)/2;
    }
    else  cost = front_cost + back_cost;
    // Check if the right cost is real small
    if (fabs(cost)<0.001){
        cost = 0.0;
    }
//    std::cout << "**** Total cost: "<<cost << std::endl;
    
    return cost;
    
}


int calculate_cost(List_Vehicle car_list, double car_s, int lane, double ref_val,bool future_s_flag = true, double detect_range = 30.0){

    float right_front_cost,right_back_cost,left_front_cost,left_back_cost = 0.0;
    double self_speed = ref_val/2.24; // Convert mph to m/s
    
    double ego_s;
    
    if (future_s_flag){
        ego_s = car_list.ego_future_s;
    }
    else{
        ego_s = car_s;
    }
    
    // Process right lane
    
    double right_cost = one_side_cost(car_list.right_front, car_list.right_back, self_speed, ego_s, detect_range, future_s_flag);
    std::cout << "**** Total Right cost: "<<right_cost << std::endl;
  
    
    double left_cost = one_side_cost(car_list.left_front, car_list.left_back, self_speed, ego_s, detect_range, future_s_flag);
    std::cout << "**** Total Left cost: "<<left_cost << std::endl;
    
    
    // Calculate same lane front car cost
    float front_cost = follow_cost(car_list.front, self_speed, ego_s, true,future_s_flag, detect_range);
    std::cout <<std::endl<< "***** front cost: "<<front_cost << std::endl;
    
    // Base on the cost decide which lane to change
    // IF both lane has no cars
    if (right_cost == 0 && right_cost == 0 ){
        // shift to right if not at the right most lane
        if (lane < 2) {
            return 2;
        }
        // if at the right most lane, turn left
        else if (lane == 2){
            return 1;
        }
    }
    // if both cost are too high stay and slow down
    else if ((right_cost > 1 && left_cost >1) || (front_cost < right_cost && front_cost < left_cost) || front_cost > 1){
        // DONT turn since it too risky
        return 0;
    }
    
    else{
        
        if (right_cost > left_cost && lane > 0){
            // more cost means good for now
            //turn left:
            return 1;
        }
        else if (right_cost < left_cost && lane < 2){
            //trun right: if the cost is same
            return 2;
        }
        else {
            // If there are equal don't change lane
            return 0;
        }
    }
}

bool safe_to_turn (List_Vehicle car_list, bool right_turn = false ){
    // Check position now to see if the gap is big
    
    if (right_turn){
        // check if it is within 15, if so, too risky to turn
        
        // Recalculate cost
//        double right_cost = one_side_cost(car_list.right_front, car_list.right_back, car_list.ego_speed, car_list.ego_s, 30, false);
        Vehicle right_front,right_back;
        if (car_list.right_front.size()>0){
            right_front = car_list.get_closest_vehicle(car_list.right_front);
        }
        else {
            right_front.s = car_list.ego_s + 100;
            right_front.future_s =car_list.ego_future_s + 100;
        }
        
        if (car_list.right_back.size()>0){
            right_back = car_list.get_closest_vehicle(car_list.right_back);
        }
        else {
            right_back.s = car_list.ego_s - 100;
            right_back.future_s =car_list.ego_future_s - 100;
        }
        
        if (right_front.s - car_list.ego_s <= 15 || car_list.ego_s - right_back.s <=15|| right_front.future_s - car_list.ego_future_s <=15 || car_list.ego_future_s - right_back.future_s <= 15){
            std::cout << "ego s:"<< car_list.ego_s << " front s:"<< right_front.s <<" back s:" << right_back.s<< std::endl;
            std::cout << "ego future s:"<< car_list.ego_s << " front future s:"<< right_front.s <<" back future s:" << right_back.s<< std::endl;
             std::cout << "!!!! NOT SAFE ON Right!!!!!" << std::endl;
            
            return false;
        }
        else{
            double side_cost = one_side_cost(car_list.right_front, car_list.right_back, car_list.ego_speed, car_list.ego_future_s, 30, true);
            double front_cost = follow_cost(car_list.front, car_list.ego_speed, car_list.ego_future_s, true);
            std::cout << "Recalculate cost  side cost:"<< side_cost<<" front cost:"<< front_cost << std::endl;
            if (side_cost < front_cost){
                return true;
            }
            else return false;
        }
        

        
    }
    else{
        
        Vehicle left_front,left_back;
        if (car_list.left_front.size()>0){
            left_front = car_list.get_closest_vehicle(car_list.left_front);
        }
        else {
            left_front.s = car_list.ego_s + 100;
            left_front.future_s = car_list.ego_future_s + 100;
        }
        if (car_list.left_back.size()>0){
            left_back = car_list.get_closest_vehicle(car_list.left_back);
        }
        else {
            left_back.s = car_list.ego_s - 100;
            left_back.future_s = car_list.ego_future_s - 100;
        }
        
        if (left_front.s - car_list.ego_s <= 15 || car_list.ego_s - left_back.s <=15 || left_front.future_s - car_list.ego_future_s <=15 || car_list.ego_future_s - left_back.future_s <= 15){
            std::cout << "ego s:"<< car_list.ego_s << " front s:"<< left_front.s <<" back s:" << left_back.s<< std::endl;
            std::cout << "ego s:"<< car_list.ego_s << " front s:"<< left_front.s <<" back s:" << left_back.s<< std::endl;
            std::cout << "!!!! NOT SAFE ON Left!!!!!" << std::endl;
            return false;
        }
        else{
            double side_cost = one_side_cost(car_list.left_front, car_list.left_back, car_list.ego_speed, car_list.ego_future_s, 30, true);
            double front_cost = follow_cost(car_list.front, car_list.ego_speed, car_list.ego_future_s, true);
            std::cout << "Recalculate cost  side cost:"<< side_cost<<" front cost:"<< front_cost << std::endl;
            if (side_cost < front_cost){
                return true;
            }
            else return false;
        }
    
    }
    // Check the cost and front cost again
    
    
}

int main() {
  uWS::Hub h;

  // Load up map values for waypoint's x,y,s and d normalized normal vectors
  vector<double> map_waypoints_x;
  vector<double> map_waypoints_y;
  vector<double> map_waypoints_s;
  vector<double> map_waypoints_dx;
  vector<double> map_waypoints_dy;

  // Waypoint map to read from
  string map_file_ = "../../data/highway_map.csv";
  // The max s value before wrapping around the track back to 0
  double max_s = 6945.554;

  std::ifstream in_map_(map_file_.c_str(), std::ifstream::in);

  string line;
  while (getline(in_map_, line)) {
    std::istringstream iss(line);
    double x;
    double y;
    float s;
    float d_x;
    float d_y;
    iss >> x;
    iss >> y;
    iss >> s;
    iss >> d_x;
    iss >> d_y;
    map_waypoints_x.push_back(x);
    map_waypoints_y.push_back(y);
    map_waypoints_s.push_back(s);
    map_waypoints_dx.push_back(d_x);
    map_waypoints_dy.push_back(d_y);
  }
    
    int lane = 1;
    double ref_val = 0.0; //MPH
    int ego_state = 0;
//    bool slow_down = false;
    bool change_lane_fail = false;
    int target_lane;
    bool lane_change_set = false;
    int fail_count = 0;
    List_Vehicle vehicle_list;

   

 h.onMessage([&vehicle_list,&fail_count,&target_lane,&lane_change_set,&change_lane_fail,&ref_val,&lane,&map_waypoints_x,&map_waypoints_y,&map_waypoints_s,
           &map_waypoints_dx,&map_waypoints_dy,&ego_state]
          (uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
           uWS::OpCode opCode) {
              
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    if (length && length > 2 && data[0] == '4' && data[1] == '2') {

      auto s = hasData(data);

      if (s != "") {
        auto j = json::parse(s);
        
        string event = j[0].get<string>();
        
        if (event == "telemetry") {
            // j[1] is the data JSON object

            // Main car's localization Data
            double car_x = j[1]["x"];
            double car_y = j[1]["y"];
            double car_s = j[1]["s"];
            double car_d = j[1]["d"];
            double car_yaw = j[1]["yaw"];
            double car_speed = j[1]["speed"];

            // Previous path data given to the Planner
            auto previous_path_x = j[1]["previous_path_x"];
            auto previous_path_y = j[1]["previous_path_y"];
            // Previous path's end s and d values
            double end_path_s = j[1]["end_path_s"];
            double end_path_d = j[1]["end_path_d"];

            // Sensor Fusion Data, a list of all other cars on the same side
            //   of the road.
            auto sensor_fusion = j[1]["sensor_fusion"];

            json msgJson;

            vector<double> next_x_vals;
            vector<double> next_y_vals;
            int prev_size = previous_path_x.size();
            bool front_car_detected = false;
            bool slow_down = false;

          /**
           * TODO: define a path made up of (x,y) points that the car will visit
           *   sequentially every .02 seconds
           */
            
           
            if (prev_size >  0) {
                car_s = end_path_s;
            }
            
            // Calculate future car_s
            vehicle_list.cal_future_ego_s(ref_val, car_s, prev_size);
            double detect_range = vehicle_list.detect_range;
            vector<Vehicle> temp_RF,temp_RB,temp_LF,temp_LB;
            // Reading sensor fusion
            for(int i = 0; i < sensor_fusion.size();++i){
                float d = sensor_fusion[i][6];
                // Find the car in the current lane in front of us
                if (d < (2+4*lane+2) && d > (2+4*lane-2) ){
                    // Find out the speed of the car
                    Vehicle car;
                    car.lane = lane;
                    car.s = sensor_fusion[i][5];
                    car.car_id = sensor_fusion[i][0];
                    car.cal_mag_v(sensor_fusion[i][3], sensor_fusion[i][4], (double) prev_size);
                    
                    // Enter 30 meters withn the front car
                    if (car.s > car_s && car.s-car_s <detect_range){
                        front_car_detected = true;
                        car.state = "F";
                        vehicle_list.follow_speed = car.speed * 2.24;
                        vehicle_list.front = car;
                        
                    }
                }
                // Find car in the right, only record cars when there is a right lane and state is not lane keep
                else if (d < (2+4*(lane+1)+2) && d > (2+4*(lane+1)-2) && lane > 0){
                    Vehicle car;
                    car.lane = lane+1;
                    car.s = sensor_fusion[i][5];
                    car.car_id = sensor_fusion[i][0];
                    car.cal_mag_v(sensor_fusion[i][3], sensor_fusion[i][4], (double) prev_size);
                    
                    if (car.s > car_s && car.s - car_s <detect_range){
                        car.state = "RF";
                        vehicle_list.append(vehicle_list.right_front, car);
                        temp_RF.push_back(car);
                    }
                    else if (car.s <= car_s && car_s - car.s <detect_range){
                        car.state = "RB";
                        vehicle_list.append(vehicle_list.right_back, car);
                        temp_RB.push_back(car);
                    }
                }
                
                // Find car in the left lane
                else if (d < (2+4*(lane-1)+2) && d > (2+4*(lane-1)-2) && lane < 2 ){
                    Vehicle car;
                    car.lane = lane-1;
                    car.s = sensor_fusion[i][5];
                    car.car_id = sensor_fusion[i][0];
                    car.cal_mag_v(sensor_fusion[i][3], sensor_fusion[i][4], (double) prev_size);
                    
                    if (car.s > car_s && car.s - car_s <detect_range){
                        car.state = "LF";
                        vehicle_list.append(vehicle_list.left_front, car);
                        temp_LF.push_back(car);
                    }
                    else if (car.s <= car_s && car_s - car.s <detect_range){
                        car.state = "LB";
                        vehicle_list.append(vehicle_list.left_back, car);
                        temp_LB.push_back(car);
                    }
                }
                
            }
            
            // Trace back missing sensor reading and delete out of range items
            vehicle_list.trace_back(vehicle_list.right_front, temp_RF, prev_size);
            vehicle_list.trace_back(vehicle_list.right_back, temp_RB, prev_size);
            vehicle_list.trace_back(vehicle_list.left_front, temp_LF, prev_size);
            vehicle_list.trace_back(vehicle_list.left_back, temp_LB, prev_size);
            
            
            // Decision making/jumping states
            switch (ego_state) {
                case 0:
                    if (front_car_detected){
                        // calculate the cost for left and right and decide which one to do
                        std::cout<< "!!!! Front car detected, Change lane fail status: "<< change_lane_fail<< std::endl;
                        int turn_result = calculate_cost(vehicle_list, car_s,lane,ref_val);
                        
                        // if there is not lane fail detect before, if there is, stay this state.
                        if (!change_lane_fail) {
                            if (turn_result == 1) {//1 left 2 right 0 not assigned
                                // Jump to PLCL
                                std::cout << "Trying left" << std::endl;
                                ego_state = 1;
                                fail_count = 0 ;
                            }
                            else if (turn_result == 2){
                                // Jumpe to PLCR
                                std::cout << "Trying right" << std::endl;
                                ego_state = 2;
                                fail_count = 0;
                            }
                            else{
                                change_lane_fail = true;
                                std::cout << "!!Change lane fail!!"<<std::endl;
                            }
                        }
                        else {
                            ego_state = 0; //stay
                            std::cout << "slowing down	********" << std::endl;
                            slow_down = true;
                        }
                    }
                    break;
                    
                // Prepare for lane change left
                case 1:
                    // check if the future position is still vaild for lane change on the left
                    // and compare to the cost of distance to the front car
                    
                    if (safe_to_turn(vehicle_list) && fail_count >= 3){
                        std::cout << "Safe to turn left" << std::endl;
                        ego_state = 3;
                        target_lane = lane - 1;
                        lane_change_set = false;
                        fail_count = 0;
                    }
                    else{
                        // stay in this state
                        // Set time if timer expired change the change lane fail and jump back to state 0
                        ego_state = 1;
                        fail_count += 1;
                        
                        if (fail_count > 10){
                            slow_down = true;
                            if (fail_count > 20){
                                change_lane_fail = true;
                                ego_state = 0;
                            }
                        }
                    
                    }
                    break;
                // Prepare for lane change right
                case 2:
                    // check if the future position is still vaild for lane change on the right
                    // and compare to the cost of distance to the front car
                    if (safe_to_turn(vehicle_list,true) && fail_count >=5){
                        std::cout << "Safe to turn right" << std::endl;
                        ego_state = 4;
                        target_lane = lane + 1;
                        lane_change_set = false;
                        fail_count = 0;
                    }
                    else{
                        // stay in this state
                        // Set time if timer expired change the change lane fail and jump back to state 0
                        ego_state = 2;
                        fail_count += 1;
                        
                        if (fail_count > 10){
                            slow_down = true;
                            if (fail_count > 20){
                                change_lane_fail = true;
                                ego_state = 0;
                            }
                        }
                        
                    }
                    break;
                // Lane chagne to left
                case 3:
                    if (car_d < (2+4*target_lane+2) && car_d > (2+4*(target_lane)-2)) {
                        ego_state = 0;
                        std::cout << "*********"<<std::endl<<"Lane shift done"<<std::endl;
                    }
                
                    break;
                // Lane chagne to right
                case 4:
                    if (car_d < (2+4*target_lane+2) && car_d > (2+4*(target_lane)-2) ) {
                        ego_state = 0;
                        std::cout << "*********"<<std::endl<<"Lane shift done"<<std::endl;
                    }
                    break;
                    
            }
            
            // Output of the State machine
            switch (ego_state) {
                // Lane Keeping
                case 0:
                    break;
                // Prepare for lane change left
                case 1:
                    std::cout << "Prepare for lane change left   Fail count:"<< fail_count << std::endl;
                    
                    break;
                // Prepare for lane change right
                case 2:
                    std::cout << "Prepare for lane change Right   Fail count:"<< fail_count << std::endl;
                    
                    break;
                // Change lane to left
                case 3:
                    // if lane change is not finished,
                    if (!lane_change_set) {
                        lane -= 1;
                        lane_change_set = true;
                    }
                    std::cout << "Moving LEFT" << std::endl;
                    break;
                // Change lane to right
                case 4:
                    if (!lane_change_set) {
                        lane += 1;
                        lane_change_set = true;
                    }
                    std::cout << "Moving Right" << std::endl;
                    break;

                    
            }
            
            if (ref_val < 49.5 && !slow_down){
                ref_val += 0.225;
            }
            else if (slow_down && ref_val > vehicle_list.follow_speed){
                ref_val -= 0.225;
                std::cout << "--ref val: " << ref_val<< std::endl;
                std::cout << "Target_follow_speed: " << vehicle_list.follow_speed<< std::endl;
                
            }
            else if (ref_val <= vehicle_list.follow_speed){
                slow_down = false;
                change_lane_fail = false;
                
                std::cout << "Reseting slow down and change_lane fail" << std::endl;
            }
            
            
            // You can move this to the N Calculation loop
            
            
            vector<double> ptsx;
            vector<double> ptsy;
            
            double ref_x = car_x;
            double ref_y = car_y;
            double ref_yaw = deg2rad(car_yaw);
            
            if(prev_size < 2){
                double prev_car_x = car_x - cos(car_yaw);
                double prev_car_y = car_y - sin(car_yaw);
                
                ptsx.push_back(prev_car_x);
                ptsx.push_back(car_x);
                
                ptsy.push_back(prev_car_y);
                ptsy.push_back(car_y);
            }
            else{
                ref_x = previous_path_x[prev_size - 1];
                ref_y = previous_path_y[prev_size - 1];
                
                double ref_x_prev = previous_path_x[prev_size-2];
                double ref_y_prev = previous_path_y[prev_size-2];
                ref_yaw = atan2(ref_y-ref_y_prev, ref_x-ref_x_prev);
                
                ptsx.push_back(ref_x_prev);
                ptsx.push_back(ref_x);
                
                ptsy.push_back(ref_y_prev);
                ptsy.push_back(ref_y);
                
            }
            vector<double> next_wp0 = getXY(car_s + 30, (2+4*lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);
            vector<double> next_wp1 = getXY(car_s + 60, (2+4*lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);
            vector<double> next_wp2 = getXY(car_s + 90, (2+4*lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);
            
            ptsx.push_back(next_wp0[0]);
            ptsx.push_back(next_wp1[0]);
            ptsx.push_back(next_wp2[0]);
            
            ptsy.push_back(next_wp0[1]);
            ptsy.push_back(next_wp1[1]);
            ptsy.push_back(next_wp2[1]);
            
            // Transfor the ptsx and y to 0 degree of yaw coordinate
            for (int i = 0; i < ptsx.size();++i){
                double shift_x = ptsx[i] - ref_x;
                double shift_y = ptsy[i] - ref_y;
                
                ptsx[i] = (shift_x * cos(0-ref_yaw)-shift_y*sin(0-ref_yaw));
                ptsy[i] = (shift_x * sin(0-ref_yaw)+shift_y*cos(0-ref_yaw));
            }
            tk::spline s;
            
            s.set_points(ptsx, ptsy);
            
            for (int i = 0; i < previous_path_x.size();++i){
                next_x_vals.push_back(previous_path_x[i]);
                next_y_vals.push_back(previous_path_y[i]);
            }
            
            double target_x = 30.0;
            double target_y = s(target_x);
            double target_dist = sqrt(target_x*target_x + target_y* target_y);
            
            double x_add_on = 0;
            
            for (int i = 1; i <= 50-previous_path_x.size(); ++i) {
                double N = (target_dist/(0.02*ref_val/2.24)); // Change mph to m/s
                double x_point = x_add_on+target_x/N;
                double y_point = s(x_point);
                
                x_add_on = x_point;
                
                double x_ref = x_point;
                double y_ref = y_point;
                
                //Rotate back to normal
                x_point = (x_ref * cos(ref_yaw)-y_ref*sin(ref_yaw));
                y_point = (x_ref * sin(ref_yaw)+y_ref*cos(ref_yaw));
                
                x_point += ref_x;
                y_point += ref_y;
                
                
                next_x_vals.push_back(x_point);
                next_y_vals.push_back(y_point);
                
            }
            
            


          msgJson["next_x"] = next_x_vals;
          msgJson["next_y"] = next_y_vals;

          auto msg = "42[\"control\","+ msgJson.dump()+"]";

          ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
        }  // end "telemetry" if
      } else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }  // end websocket if
  }); // end h.onMessage

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  
  h.run();
}
