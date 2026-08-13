#include "control.h"
#include "math.h"
using namespace Control;
#define rad_to_degree 57.30f
// cm
float R1=15.0f ;
float R2=16.5f ;
float R_all=  sqrt(R1 * R1 + R2 * R2);
float tmep_m_1;
float tmep_m_2;


// x，y应该输入cm（0，31.5cm）
void Control::Control_t::controlUpdate(float x, float z, float yaw)
{
    _x = x;
    _z = z;
    _yaw = yaw;
    float θ_t = 0;
    float θ_1_t = 0;
    float θ_2_t = 0;
	  float  sqrt_xz = sqrt(_x*_x+_z*_z);
	float D=  (sqrt_xz*sqrt_xz+R2*R2-R1*R1)/(2*R2*sqrt_xz);
	float D_B=  (sqrt_xz*sqrt_xz+R1*R1-R2*R2)/(2*R2*sqrt_xz);
    if (x == 0 && z == 0)
    {
        (_servo_theta1)->_target_angle = (_servo_theta1)->_zero_angle;
        (_servo_theta2)->_target_angle = (_servo_theta2)->_zero_angle;
    }
    else
    {
        if (x > 0 && z > -5)
        {

            θ_t = atan(_z / _x);
					
//有小问题		
	      θ_2_t=atan2(-_z,_x)+acos(D);
				θ_1_t=atan2(_z,_x)+acos(D_B);
					
				float	 tmep_x= _x/100.0f;
				float		temp_z= _z/100.0f;                                   
			if((100*tmep_x*tmep_x + 100*temp_z*temp_z)<9)		
			{
			 tmep_m_1=(2*atan((3*temp_z - sqrt(-(tmep_x*tmep_x + temp_z*temp_z)*(100*tmep_x*tmep_x + 100*temp_z*temp_z- 9)))/(10*tmep_x*tmep_x - 3*tmep_x + 10*temp_z*temp_z)))*rad_to_degree;		
			tmep_m_2=(-2*atan((3*temp_z - sqrt(-(tmep_x*tmep_x + temp_z*temp_z)*(100*tmep_x*tmep_x + 100*temp_z*temp_z - 9)))/(10*tmep_x*tmep_x + 3*tmep_x + 10*temp_z*temp_z)))*rad_to_degree;
			}
//(_servo_theta1)->_target_angle =-2*atan(((31*(3200*z - sqrt(-(10000*(x*x) + 10000*(z*z) - 1)*(10000*(x*x) + 10000*(z*z) - 961))))/(10000*(x*x) + 3200*x + 10000*(z*z) + 31) - 200*z + (10000*(x*x)*(3200*z - (-(10000*(x*x) + 10000*(z*z) - 1)*(10000*(x*x) + 10000*(z*z) - 961))^(1/2)))/(10000*(x*x) + 3200*x + 10000*(z*z) + 31) + (10000*(z*z)*(3200*z - sqrt(-(10000*(x*x) + 10000*(z*z) - 1)*(10000*(x*x) + 10000*(z*z) - 961))))/(10000*(x*x) + 3200*x + 10000*(z*z) + 31) + (3200*x*(3200*z - sqrt(-(10000*(x*x) + 10000*(z*z) - 1)*(10000*(x*x) + 10000*(z*z) - 961))))/(10000*(x*x) + 3200*x + 10000*(z*z) + 31))/(- 10000*(x*x) + 3000*x - 10000*(z*z) + 31));
// (_servo_theta2)->_target_angle=-2*atan((3200*z - sqrt(-(10000*(x*x) + 10000*(z*z) - 1)*(10000*(x*x) + 10000*(z*z) - 961)))/(10000*(x*x) + 3200*x + 10000*(z*z) + 31))-2*atan((3200*z + (-(10000*(x*x) + 10000*(z*z) - 1)*(10000*(x*x) + 10000*(z*z) - 961))^(1/2))/(10000*(x*x) + 3200*x + 10000*(z*z) + 31));					

            if (θ_1_t > (_servo_theta1)->_use_max_angle)
            {

                (_servo_theta1)->_target_angle = (_servo_theta1)->_use_max_angle;
            }
            else if (θ_1_t < (_servo_theta1)->_use_min_angle)
            {
                (_servo_theta1)->_target_angle = (_servo_theta1)->_use_min_angle;
            }
            else
            {
//                (_servo_theta1)->_target_angle = θ_1_t*rad_to_degree;
									  (_servo_theta1)->_target_angle=  tmep_m_1;
            }

            if (θ_2_t > (_servo_theta2)->_use_max_angle)
            {
                (_servo_theta2)->_target_angle = (_servo_theta2)->_use_max_angle;
            }
            else if (θ_2_t < (_servo_theta2)->_use_min_angle)
            {
                (_servo_theta2)->_target_angle = (_servo_theta2)->_use_min_angle;
            }
            else
            {
//                (_servo_theta2)->_target_angle = θ_2_t*rad_to_degree;
							(_servo_theta2)->_target_angle =tmep_m_2;
            }
        }
    }
}
