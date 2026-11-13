#include <iostream>
using namespace std;
 
bool isEven(int num) {
    return num % 2 == 0;
}
 
int main() {
    if (isEven(14)) {
        cout << "Even" << endl;
    } else {
        cout << "Odd" << endl;
    }
    return 0;
}